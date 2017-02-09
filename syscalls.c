/*
  This file is part of MAMBO, a low-overhead dynamic binary modification tool:
      https://github.com/beehive-lab/mambo

  Copyright 2013-2017 Cosmin Gorgovan <cosmin at linux-geek dot org>

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <stdio.h>
#include <asm/unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/sched.h>
#include <assert.h>
#include <limits.h>
#include <string.h>

#include "dbm.h"

#ifdef DEBUG
  #define debug(...) fprintf(stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

#ifdef __aarch64__
  #define SIG_FRAG_OFFSET 4
#else
  #define SIG_FRAG_OFFSET 0
#endif

void *dbm_start_thread_pth(void *ptr) {
  dbm_thread *thread_data = (dbm_thread *)ptr;
  assert(thread_data->clone_args->child_stack);

  current_thread = thread_data;
  uintptr_t addr = scan(thread_data, thread_data->clone_ret_addr, ALLOCATE_BB);
  pid_t tid = syscall(__NR_gettid);

  if (thread_data->clone_args->flags & CLONE_PARENT_SETTID) {
    *thread_data->clone_args->ptid = tid;
  }
  if (thread_data->clone_args->flags & CLONE_CHILD_SETTID) {
    *thread_data->clone_args->ctid = tid;
  }
  if (thread_data->clone_args->flags & CLONE_CHILD_CLEARTID) {
		syscall(__NR_set_tid_address, thread_data->clone_args->ctid);
  }
  thread_data->tls = thread_data->clone_args->tls;

  // Copy the parent's saved register values to the child's stack
#ifdef __arm__
  uint32_t *child_stack = thread_data->clone_args->child_stack;
  child_stack -= 15; // reserve 15 words on the child's stack
  mambo_memcpy(child_stack, thread_data->clone_args, sizeof(uint32_t) * 14);
  child_stack[r0] = 0; // return 0
#endif
#ifdef __aarch64__
  uint64_t *child_stack = thread_data->clone_args->child_stack;
  child_stack -= 34;
  mambo_memcpy(child_stack, (void *)thread_data->clone_args, sizeof(uint64_t) * 32);
  // move the values for X0 and X1 to the bottom of the stack
  child_stack[32] = 0; // X0
  child_stack[33] = child_stack[1]; // X1
  child_stack += 2;
#endif

  // Release the lock
  __asm__ volatile("dmb sy");
  thread_data->tid = tid;

  th_enter(child_stack, addr);
  return NULL;
}

dbm_thread *dbm_create_thread(dbm_thread *thread_data, void *next_inst, sys_clone_args *args) {
  pthread_t thread;
  dbm_thread *new_thread_data;

  if (!allocate_thread_data(&new_thread_data)) {
    fprintf(stderr, "Failed to allocate thread data\n");
    while(1);
  }
  init_thread(new_thread_data);
  new_thread_data->clone_ret_addr = next_inst;
  new_thread_data->tid = 0;
  new_thread_data->clone_args = args;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
  /* We're switching to the stack allocated by the application immediately, so make this
     as small as possible. Our glibc stores data here, so we can't unmap it.
     Also see man pthread_attr_setguardsize BUGS. */
  pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + 4096);
  pthread_attr_setguardsize(&attr, 4096);
  pthread_create(&thread, &attr, dbm_start_thread_pth, new_thread_data);

  return new_thread_data;
}


// return 0 to skip the syscall
int syscall_handler_pre(uintptr_t syscall_no, uintptr_t *args, uint16_t *next_inst, dbm_thread *thread_data) {
  struct sigaction *sig_action;
  sys_clone_args *clone_args;
  debug("syscall pre %d\n", syscall_no);

#ifdef PLUGINS_NEW
  mambo_context ctx;
  int cont;

  if (global_data.free_plugin > 0) {
    set_mambo_context(&ctx, thread_data, -1, -1, -1, -1, -1, NULL, NULL, (unsigned long *)args);
    for (int i = 0; i < global_data.free_plugin; i++) {
      if (global_data.plugins[i].cbs[PRE_SYSCALL_C] != NULL) {
        ctx.plugin_id = i;
        cont = global_data.plugins[i].cbs[PRE_SYSCALL_C](&ctx);
        if (!cont) return 0;
      } // if
    } // for
  }
#endif

  switch(syscall_no) {
    case __NR_clone:
      clone_args = (sys_clone_args *)args;

      if (clone_args->flags & CLONE_VFORK) {
        assert(clone_args->child_stack == &args[32]
               || clone_args->child_stack == NULL);
        clone_args->child_stack = args;
        clone_args->flags &= ~CLONE_VM;
      }
      if (clone_args->flags & CLONE_VM) {
        if (!(clone_args->flags & CLONE_SETTLS)) {
          clone_args->tls = thread_data->tls;
        }
        thread_data->clone_vm = true;

        dbm_thread *child_data = dbm_create_thread(thread_data, next_inst, clone_args);
        while(child_data->tid == 0);
        args[0] = child_data->tid;

        return 0;
      } else {
        thread_data->child_tls = (clone_args->flags & CLONE_SETTLS) ? clone_args->tls : thread_data->tls;
        clone_args->flags &= ~CLONE_SETTLS;

        thread_data->clone_vm = false;
      }
      break;
    case __NR_exit:
      debug("thread exit\n");
#ifdef PLUGINS_NEW
      mambo_deliver_callbacks(POST_THREAD_C, thread_data, -1, -1, -1, -1, -1, NULL, NULL, NULL);
#endif
      if (munmap(thread_data->code_cache, CC_SZ_ROUND(sizeof(dbm_code_cache))) != 0) {
        fprintf(stderr, "Error freeing code cache on exit()\n");
        while(1);
      }
      if (munmap(thread_data, METADATA_SZ_ROUND(sizeof(dbm_thread))) != 0) {
        fprintf(stderr, "Error freeing thread private structure on exit()\n");
        while(1);
      }
      pthread_exit(NULL); // this should never return
      while(1); 
      break;
    case __NR_rt_sigaction:
      debug("sigaction %d\n", args[0]);
      debug("struct sigaction at 0x%x\n", args[1]);
      sig_action = (struct sigaction *)args[1];
      // If act is non-NULL, the new action for signal signum is installed from act. If oldact is non-NULL, the previous action is saved in oldact.
      debug("handler at %p\n", sig_action->sa_handler);
      if (sig_action
          && sig_action->sa_handler != SIG_IGN
          && sig_action->sa_handler != SIG_DFL) {
        sig_action->sa_handler = (void *)lookup_or_scan(thread_data, (uintptr_t)sig_action->sa_handler, NULL)
                                 + SIG_FRAG_OFFSET;
      }
      break;
    case __NR_exit_group:
      dbm_exit(thread_data, args[0]);
      break;
    case __NR_close:
      if (args[0] <= 2) { // stdin, stdout, stderr
        args[0] = 0;
        return 0;
      }
      break;
    /* Remove the execute permission from application mappings. At this point, this mostly acts
       as a safeguard in case a translation bug causes a branch to unmodified application code.
       Page permissions happen to be passed in the third argument both for mmap and mprotect. */
#ifdef __arm__
    case __NR_mmap2:
#endif
    case __NR_mprotect:
      /* Ensure that code pages are readable by the code scanner. */
      if (args[2] & PROT_EXEC) {
        assert(args[2] & PROT_READ);
      }
      args[2] &= ~PROT_EXEC;
      break;

    case __NR_munmap:
      flush_code_cache(thread_data);
      break;

#ifdef __arm__
    case __NR_vfork:
      assert(thread_data->is_vfork_child == false);
      thread_data->is_vfork_child = true;
      for (int i = 0; i < 3; i++) {
        thread_data->parent_scratch_regs[i] = thread_data->scratch_regs[i];
      }
      break;
    case __ARM_NR_cacheflush:
      /* Returning to the calling BB is potentially unsafe because the remaining
         contents of the BB or other basic blocks it is linked against could be stale */
      flush_code_cache(thread_data);
      break;
    case __ARM_NR_set_tls:
      debug("set tls to %x\n", args[0]);
      thread_data->tls = args[0];
      args[0] = 0;
      return 0;
      break;
#endif
  }
  
  return 1;
}

void syscall_handler_post(uintptr_t syscall_no, uintptr_t *args, uint16_t *next_inst, dbm_thread *thread_data) {
  dbm_thread *new_thread_data;
  
  debug("syscall post %d\n", syscall_no);

  switch(syscall_no) {
    case __NR_clone:
      debug("r0 (tid): %d\n", args[0]);
      if (args[0] == 0) { // the child
        assert(!thread_data->clone_vm);
        /* Without CLONE_VM, the child runs in a separate memory space,
           no synchronisation is needed.*/
        thread_data->tls = thread_data->child_tls;
      }
      break;

#ifdef __arm__
    case __NR_vfork:
      if (args[0] != 0) { // in the parent
        for (int i = 0; i < 3; i++) {
          thread_data->scratch_regs[i] = thread_data->parent_scratch_regs[i];
        }
        thread_data->is_vfork_child = false;
      }
      break;
#endif
  }

#ifdef PLUGINS_NEW
  mambo_deliver_callbacks(POST_SYSCALL_C, thread_data, -1, -1, -1, -1, -1, NULL, NULL, (unsigned long *)args);
#endif
}
