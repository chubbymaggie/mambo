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
#include <limits.h>

#include "dbm.h"
#include "scanner_common.h"
#include "pie/pie-thumb-encoder.h"
#include "pie/pie-arm-encoder.h"

#ifdef DEBUG
  #define debug(...) fprintf(stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

void dispatcher(uintptr_t target, uintptr_t *next_addr, uint32_t source_index, dbm_thread *thread_data) {
  uintptr_t block_address;
  uintptr_t other_target;
  uintptr_t pred_target;
  uint16_t *branch_addr; 
  uint32_t *branch_table;
  bool     cached;
  bool     other_target_in_cache;
  int      cache_index;
  uint8_t  *table;
  branch_type source_branch_type;
  uint8_t  sr[2];
  uint32_t reglist;
  uint32_t *ras;
  bool use_bx_lr;
  int unwind_len;
  bool is_taken;

/* It's essential to copy exit_branch_type before calling lookup_or_scan
     because when scanning a stub basic block the source block and its
     meta-information get overwritten */
  debug("Source block index: %d\n", source_index);
  source_branch_type = thread_data->code_cache_meta[source_index].exit_branch_type;

#ifdef DBM_TRACES
  // Handle trace exits separately
  if (source_index >= CODE_CACHE_SIZE && source_branch_type != tbb && source_branch_type != tbh) {
    return trace_dispatcher(target, next_addr, source_index, thread_data);
  }
#endif
  
  debug("Reached the dispatcher, target: 0x%x, ret: %p, src: %d thr: %p\n", target, next_addr, source_index, thread_data);
  block_address = lookup_or_scan(thread_data, target, &cached);
  if (cached) {
    debug("Found block from %d for 0x%x in cache at 0x%x\n", source_index, target, block_address);
  } else {
    debug("Scanned at 0x%x for 0x%x\n", block_address, target);
  }
  
  switch (source_branch_type) {
#ifdef DBM_TB_DIRECT
    case tbb:
    case tbh:
      /* the index is invalid only when the inline hash lookup is called for a new BB,
         no linking is required */
#ifdef FAST_BT
      if (thread_data->code_cache_meta[source_index].rn >= TB_CACHE_SIZE) {
        break;
      }
#else
      if (thread_data->code_cache_meta[source_index].rn >= MAX_TB_INDEX) {
        break;
      }
#endif
      //thread_data->code_cache_meta[source_index].count++;
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
#ifdef FAST_BT
      branch_table = (uint32_t *)(((uint32_t)branch_addr + 20 + 2) & 0xFFFFFFFC);
      branch_table[thread_data->code_cache_meta[source_index].rn] = block_address;
#else
      branch_addr += 7;
      table = (uint8_t *)branch_addr;
      if (thread_data->code_cache_meta[source_index].free_b == TB_CACHE_SIZE) {
        // if the list of linked blocks is full, link this index to the inline hash lookup
  #ifdef DBM_D_INLINE_HASH
        table[thread_data->code_cache_meta[source_index].rn] = MAX_TB_INDEX / 2 + TB_CACHE_SIZE * 2 + 1;
  #else
        table[thread_data->code_cache_meta[source_index].rn] = MAX_TB_INDEX / 2 + TB_CACHE_SIZE * 2;
  #endif
      } else {
        // allocate a branch slot and link it
        cache_index = thread_data->code_cache_meta[source_index].free_b++;
        table[thread_data->code_cache_meta[source_index].rn] = MAX_TB_INDEX / 2 + cache_index * 2;
        
        // insert the branch to the target BB
        branch_addr += MAX_TB_INDEX / 2 + cache_index * 2;
        thumb_cc_branch(thread_data, branch_addr, (uint32_t)block_address);
        __clear_cache(branch_addr, branch_addr + 5);
      }
  #endif
      
      // invalidate the saved rm value - required to detect calls from the inline hash lookup
      thread_data->code_cache_meta[source_index].rn = INT_MAX;

      break;
#endif
#ifdef DBM_LINK_UNCOND_IMM
    case uncond_imm_thumb:
    case uncond_b_to_bl_thumb:
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
      if (block_address & 0x1) {
        if (source_branch_type == uncond_b_to_bl_thumb) {
          thumb_b32_helper(branch_addr, (uint32_t)block_address);
        } else {
          thumb_cc_branch(thread_data, branch_addr, (uint32_t)block_address);
        }
        __clear_cache((char *)branch_addr-1, (char *)(branch_addr) + 8);
      } else {
        // The data word used for the address is word-aligned
        if (((uint32_t)branch_addr) & 2) {
          thumb_ldrl32(&branch_addr, pc, 4, 1);
          branch_addr += 3;
        } else {
          thumb_ldrl32(&branch_addr, pc, 0, 1);
          branch_addr += 2;
        }
        *(uint32_t *)branch_addr = block_address;
        __clear_cache((char *)branch_addr-7, (char *)branch_addr);
      }
      break;

    case uncond_imm_arm:
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
      arm_b32_helper((uint32_t *)branch_addr, (uint32_t)block_address, AL);
      __clear_cache(branch_addr, (char *)branch_addr+5);
      break;
#endif
#ifdef DBM_LINK_COND_IMM
    case cond_imm_arm:
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
      is_taken = target == thread_data->code_cache_meta[source_index].branch_taken_addr;
      if (is_taken) {
        other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_skipped_addr);
        other_target_in_cache = (other_target != UINT_MAX);

        if (thread_data->code_cache_meta[source_index].branch_cache_status & 1) {
          branch_addr += 2;
        }

        arm_cc_branch(thread_data, (uint32_t *)branch_addr, (uint32_t)block_address,
                      thread_data->code_cache_meta[source_index].branch_condition);
      } else {
        other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_taken_addr);
        other_target_in_cache = (other_target != UINT_MAX);

        if (thread_data->code_cache_meta[source_index].branch_cache_status & 2) {
          branch_addr += 2;
        }

        arm_cc_branch(thread_data, (uint32_t *)branch_addr, (uint32_t)block_address,
                       arm_inverse_cond_code[thread_data->code_cache_meta[source_index].branch_condition]);
      }
      thread_data->code_cache_meta[source_index].branch_cache_status |= is_taken ? 2 : 1;

      if (other_target_in_cache &&
          (thread_data->code_cache_meta[source_index].branch_cache_status & (is_taken ? 1 : 2)) == 0) {
        branch_addr += 2;
        arm_cc_branch(thread_data, (uint32_t *)branch_addr, (uint32_t)other_target, AL);

        thread_data->code_cache_meta[source_index].branch_cache_status |= is_taken ? 1 : 2;
      }

      __clear_cache((char *)branch_addr-4, (char *)branch_addr+8);
      break;

    case cond_imm_thumb:
      if (block_address & 0x1) {
        branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
        debug("Target is: 0x%x, b taken addr: 0x%x, b skipped addr: 0x%x\n",
               target, thread_data->code_cache_meta[source_index].branch_taken_addr,
               thread_data->code_cache_meta[source_index].branch_skipped_addr);
        debug("Overwriting branches at %p\n", branch_addr);
        if (target == thread_data->code_cache_meta[source_index].branch_taken_addr) {
          other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_skipped_addr);
          other_target_in_cache = (other_target != UINT_MAX);
          thumb_encode_cond_imm_branch(thread_data, &branch_addr, 
                                      source_index,
                                      block_address,
                                      (other_target_in_cache ? other_target : thread_data->code_cache_meta[source_index].branch_skipped_addr),
                                      thread_data->code_cache_meta[source_index].branch_condition,
                                      true,
                                      other_target_in_cache, true);
        } else {
          other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_taken_addr);
          other_target_in_cache = (other_target != UINT_MAX);
          thumb_encode_cond_imm_branch(thread_data, &branch_addr, 
                                      source_index,
                                      (other_target_in_cache ? other_target : thread_data->code_cache_meta[source_index].branch_taken_addr),
                                      block_address,
                                      thread_data->code_cache_meta[source_index].branch_condition,
                                      other_target_in_cache,
                                      true, true);
        }
        debug("Target at 0x%x, other target at 0x%x\n", block_address, other_target);
        // thumb_encode_cond_imm_branch updates branch_addr to point to the next free word
        __clear_cache((char *)(branch_addr)-100, (char *)branch_addr);
      } else {
        fprintf(stderr, "WARN: cond_imm_thumb to arm\n");
        while(1);
      }
      break;
#endif
#ifdef DBM_LINK_CBZ
    case cbz_thumb:
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;
      debug("Target is: 0x%x, b taken addr: 0x%x, b skipped addr: 0x%x\n",
             target, thread_data->code_cache_meta[source_index].branch_taken_addr,
             thread_data->code_cache_meta[source_index].branch_skipped_addr);
      debug("Overwriting branches at %p\n", branch_addr);
      if (target == thread_data->code_cache_meta[source_index].branch_taken_addr) {
        other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_skipped_addr);
        other_target_in_cache = (other_target != UINT_MAX);
        thumb_encode_cbz_branch(thread_data,
                                thread_data->code_cache_meta[source_index].rn,
                                &branch_addr,
                                source_index,
                                block_address,
                                (other_target_in_cache ? other_target : thread_data->code_cache_meta[source_index].branch_skipped_addr),
                                true,
                                other_target_in_cache, true);
      } else {
        other_target = hash_lookup(&thread_data->entry_address, thread_data->code_cache_meta[source_index].branch_taken_addr);
        other_target_in_cache = (other_target != UINT_MAX);
        thumb_encode_cbz_branch(thread_data,
                                thread_data->code_cache_meta[source_index].rn,
                                &branch_addr,
                                source_index,
                                (other_target_in_cache ? other_target : thread_data->code_cache_meta[source_index].branch_taken_addr),
                                block_address,
                                other_target_in_cache,
                                true, true);
      }
      debug("Target at 0x%x, other target at 0x%x\n", block_address, other_target);
      // tthumb_encode_cbz_branch updates branch_addr to point to the next free word
      __clear_cache((char *)(branch_addr)-100, (char *)branch_addr);
      break;
#endif

    case uncond_blxi_thumb:
      branch_addr = thread_data->code_cache_meta[source_index].exit_branch_addr;

      thumb_ldrl32(&branch_addr, pc, ((uint32_t)branch_addr & 2) ? 4 : 0, 1);
	    branch_addr += 2;
	    // The target is word-aligned
	    if ((uint32_t)branch_addr & 2) { branch_addr++; }
	    *(uint32_t *)branch_addr = block_address;
	    __clear_cache((char *)(branch_addr)-6, (char *)branch_addr);

	    record_cc_link(thread_data, (uint32_t)branch_addr|FULLADDR, block_address);

      break;
  }
  
  *next_addr = block_address;
}
