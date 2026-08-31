#ifndef TEST_HPA_POOL_H
#define TEST_HPA_POOL_H

#include "jemalloc/internal/hpa_pool.h"

/*
 * Also exported for test: the shard-count clamp is pure configuration
 * arithmetic.
 */
extern void hpa_pool_configs_clamp(hpa_pool_configs_t *configs);

#endif /* TEST_HPA_POOL_H */
