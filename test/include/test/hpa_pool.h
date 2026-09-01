#ifndef TEST_HPA_POOL_H
#define TEST_HPA_POOL_H

#include "jemalloc/internal/hpa_pool.h"

/*
 * Static in production builds (JET_EXTERN); exported so the routing tests can
 * build a table with the real code rather than a copy of it.  Reimplementing
 * the construction in the test would make the table assertions compare the
 * test against itself.
 */
extern void hpa_pool_build_route_table(hpa_pool_set_t *set);

/*
 * Also exported for test: the shard-count clamp is pure arithmetic on a
 * layout, and it guards an invariant (every shard id must fit in the extent's
 * owner field) whose violation is silent in a release build.
 */
extern void hpa_pool_layout_clamp(hpa_pool_layout_t *layout);

/*
 * And the validator, for the same reason: it is the only thing standing
 * between a mistyped MALLOC_CONF and a routing table with a hole in it, and a
 * hole is correctness-preserving -- those sizes just go to the PAC -- so
 * nothing else would notice.
 */
extern bool hpa_pool_layout_validate(const hpa_pool_layout_t *layout);

#endif /* TEST_HPA_POOL_H */
