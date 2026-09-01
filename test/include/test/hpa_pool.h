#ifndef TEST_HPA_POOL_H
#define TEST_HPA_POOL_H

#include "jemalloc/internal/hpa_pool.h"

/*
 * Also exported for test: the shard-count clamp is pure configuration
 * arithmetic.
 */
extern void hpa_pool_configs_clamp(hpa_pool_configs_t *configs);

/*
 * And the validator, for the same reason: it is the only thing standing
 * between a mistyped MALLOC_CONF and a routing table with a hole in it, and a
 * hole is correctness-preserving -- those sizes just go to the PAC -- so
 * nothing else would notice.
 */
extern bool hpa_pool_configs_validate(const hpa_pool_configs_t *configs);

#endif /* TEST_HPA_POOL_H */
