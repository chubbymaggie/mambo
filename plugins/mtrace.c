/*
  This file is part of MAMBO, a low-overhead dynamic binary modification tool:
      https://github.com/beehive-lab/mambo

  Copyright 2017 Cosmin Gorgovan <cosmin at linux-geek dot org>

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

#ifdef PLUGINS_NEW

#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include "../plugins.h"

#include "mtrace.h"

struct mtrace {
  uint32_t len;
  uintptr_t entries[BUFLEN];
};

extern void mtrace_print_buf_trampoline(struct mtrace *trace);
extern void mtrace_buf_write(uintptr_t value, struct mtrace *trace);

void mtrace_print_buf(struct mtrace *mtrace_buf) {
  for (int i = 0; i < mtrace_buf->len; i++) {
    /* Warning: this is very slow
       For practical use, you are encouraged to process the data in memory
       or write the trace in the raw binary format */
    fprintf(stderr, "%p\n", (void *)mtrace_buf->entries[i]);
  }
  mtrace_buf->len = 0;
}

int mtrace_pre_inst_handler(mambo_context *ctx) {
  struct mtrace *mtrace_buf = mambo_get_thread_plugin_data(ctx);
  if (mambo_is_load_or_store(ctx)) {
    emit_push(ctx, (1 << 0) | (1 << 1) | (1 << 2) | (1 << lr));    
    int ret = mambo_calc_ld_st_addr(ctx, 0);
    assert(ret == 0);
    emit_set_reg_ptr(ctx, 1, &mtrace_buf->entries);
    emit_fcall(ctx, mtrace_buf_write);
    emit_pop(ctx, (1 << 0) | (1 << 1) | (1 << 2) | (1 << lr));
  }
}

int mtrace_pre_thread_handler(mambo_context *ctx) {
  struct mtrace *mtrace_buf = mambo_alloc(ctx, sizeof(*mtrace_buf));
  assert(mtrace_buf != NULL);
  mtrace_buf->len = 0;

  int ret = mambo_set_thread_plugin_data(ctx, mtrace_buf);
  assert(ret == MAMBO_SUCCESS);
}

int mtrace_post_thread_handler(mambo_context *ctx) {
  struct mtrace *mtrace_buf = mambo_get_thread_plugin_data(ctx);
  mtrace_print_buf(mtrace_buf);
  mambo_free(ctx, mtrace_buf);
}

__attribute__((constructor)) void mtrace_init_plugin() {
  mambo_context *ctx = mambo_register_plugin();
  assert(ctx != NULL);

  mambo_register_pre_thread_cb(ctx, &mtrace_pre_thread_handler);
  mambo_register_post_thread_cb(ctx, &mtrace_post_thread_handler);
  mambo_register_pre_inst_cb(ctx, &mtrace_pre_inst_handler);
}
#endif
