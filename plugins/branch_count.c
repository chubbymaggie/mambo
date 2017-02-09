#ifdef PLUGINS_NEW

#include <stdio.h>
#include <assert.h>
#include <locale.h>
#include "../plugins.h"

struct br_count {
  uint64_t direct_branch_count;
  uint64_t indirect_branch_count;
  uint64_t return_branch_count;
};

int branch_count_pre_thread_handler(mambo_context *ctx) {
  struct br_count *counters = mambo_alloc(ctx, sizeof(struct br_count));
  assert(counters != NULL);
  mambo_set_thread_plugin_data(ctx, counters);

  counters->direct_branch_count = 0;
  counters->indirect_branch_count = 0;
  counters->return_branch_count = 0;
}

int branch_count_post_thread_handler(mambo_context *ctx) {
  struct br_count *counters = mambo_get_thread_plugin_data(ctx);

  fprintf(stderr, "direct branches: %'lld\n", counters->direct_branch_count);
  fprintf(stderr, "indirect branches: %'lld\n", counters->indirect_branch_count);
  fprintf(stderr, "returns: %'lld\n\n", counters->return_branch_count);

  mambo_free(ctx, counters);
}

int branch_count_pre_inst_handler(mambo_context *ctx) {
  struct br_count *counters = mambo_get_thread_plugin_data(ctx);
  uint64_t *counter = NULL;

  mambo_branch_type type = mambo_get_branch_type(ctx);
  if (type & BRANCH_RETURN) {
    counter = &counters->return_branch_count;
  } else if (type & BRANCH_DIRECT) {
    counter = &counters->direct_branch_count;
  } else if (type & BRANCH_INDIRECT) {
    counter = &counters->indirect_branch_count;
  }

  if (counter != NULL) {
    emit_counter64_incr(ctx, counter, 1);
  }
}

__attribute__((constructor)) void branch_count_init_plugin() {
  mambo_context *ctx = mambo_register_plugin();
  assert(ctx != NULL);

  mambo_register_pre_inst_cb(ctx, &branch_count_pre_inst_handler);
  mambo_register_pre_thread_cb(ctx, &branch_count_pre_thread_handler);
  mambo_register_post_thread_cb(ctx, &branch_count_post_thread_handler);

  setlocale(LC_NUMERIC, "");
}
#endif
