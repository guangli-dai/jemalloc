#ifndef JEMALLOC_INTERNAL_HPA_POOL_H
#define JEMALLOC_INTERNAL_HPA_POOL_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/psset.h"
#include "jemalloc/internal/sz.h"

/*
 * HPA shard pools.
 *
 * Allocation maps the requested allocation size class to a pool, then uses
 * the pool's picker policy to select a shard.  Deallocation returns an extent
 * to the shard recorded on its pageslab.
 */

/* How a pool chooses one of its shard slots. */
typedef enum hpa_pool_pick_e {
	/*
	 * slot = hint % nshards, where the hint is the requesting arena index.
	 */
	hpa_pool_pick_arena = 0,
	/* Atomic counter.  Spreads load without reference to arena identity. */
	hpa_pool_pick_roundrobin = 1,
	hpa_pool_pick_limit
} hpa_pool_pick_t;

/* Configuration names indexed by hpa_pool_pick_t. */
extern const char *const hpa_pool_pick_type_names[];

/*
 * Routing key domain: one key per size class.
 *
 * The key is the size class of the size the CALLER ASKED FOR, not of the
 * extent that ends up backing it.  See hpa_route_key() for why.
 */
#define HPA_ROUTE_NKEYS SC_NSIZES

/* A cap on pools, purely so the set can be a fixed-size struct. */
#define HPA_MAX_POOLS 8

/*
 * Each pool owns this many stable shard slots.  Slots start NULL, so shard
 * metadata is allocated only when a slot is first selected.
 */
#define HPA_MAX_SHARDS_PER_POOL 1024

typedef struct hpa_pool_s hpa_pool_t;
typedef struct hpa_pool_set_s hpa_pool_set_t;
struct hpa_pool_s {
	/* Inclusive requested-allocation-size upper bound. */
	size_t size_max;
	szind_t szind_max;

	/* Number of slots available to the picker. */
	unsigned nshards;
	hpa_pool_pick_t pick;
	/* Stable position in the containing pool set. */
	unsigned pool_ind;
	hpa_pool_set_t *set;

	atomic_p_t shards[HPA_MAX_SHARDS_PER_POOL];

	/* Serializes initialization of NULL shard slots. */
	malloc_mutex_t init_mtx;
	/* Kept separate from the read-mostly picker fields. */
	JEMALLOC_ALIGNED(CACHELINE) atomic_u_t rr_next;
};

/* Configuration used to construct a pool set. */
typedef struct hpa_pool_config_s hpa_pool_config_t;
struct hpa_pool_config_s {
	/* Inclusive requested-allocation-size upper bound. */
	size_t size_max;
	unsigned nshards;
};

typedef struct hpa_pool_configs_s hpa_pool_configs_t;
struct hpa_pool_configs_s {
	unsigned npools;
	hpa_pool_config_t pools[HPA_MAX_POOLS];
	hpa_pool_pick_t pick;
};

struct hpa_pool_set_s {
	bool initialized;
	unsigned npools;

	hpa_pool_t pools[HPA_MAX_POOLS];

	/* Shared inputs retained for lazy shard initialization. */
	base_t *base;
	hpa_central_t *central;
	emap_t *emap;
	edata_cache_t *edata_cache;
	hpa_shard_opts_t opts;
	sec_opts_t sec_opts;
};

/* Stable pageslab owner id for a pool-local shard slot. */
static inline unsigned
hpa_pool_shard_id(const hpa_pool_t *pool, unsigned shard_ind) {
	assert(pool->pool_ind < HPA_MAX_POOLS);
	assert(shard_ind < HPA_MAX_SHARDS_PER_POOL);
	return pool->pool_ind * HPA_MAX_SHARDS_PER_POOL + shard_ind;
}

/* Resolves an owner id recorded on a pageslab. */
static inline hpa_shard_t *
hpa_pool_shard_from_id(hpa_pool_set_t *set, unsigned id) {
	unsigned pool_ind = id / HPA_MAX_SHARDS_PER_POOL;
	unsigned shard_ind = id % HPA_MAX_SHARDS_PER_POOL;
	if (pool_ind >= set->npools ||
	    shard_ind >= set->pools[pool_ind].nshards) {
		return NULL;
	}
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &set->pools[pool_ind].shards[shard_ind], ATOMIC_ACQUIRE);
	return shard;
}

/*
 * The identity configuration: one pool spanning all requested sizes, with
 * arena-based shard selection.  Returns true if narenas does not fit in the
 * fixed shard array; an identity configuration is not produced in that case.
 */
bool hpa_pool_configs_identity(hpa_pool_configs_t *configs, unsigned narenas);

bool hpa_pool_configs_validate(const hpa_pool_configs_t *configs);

/*
 * Build the pool set.  Returns true on error, having emitted a message.  The
 * caller supplies the metadata base and the edata cache that every shard will
 * draw from; both must outlive every arena, since a pooled extent can be
 * created by one arena and freed by another.
 */
bool hpa_pools_boot(tsdn_t *tsdn, hpa_pool_set_t *set, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_pool_configs_t *configs, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts);

/* The routing key is the requested allocation size class. */
JEMALLOC_ALWAYS_INLINE unsigned
hpa_route_key(size_t size, bool slab, szind_t szind) {
	(void)size;
	(void)slab;
	assert(size <= HUGEPAGE);
	assert(szind < SC_NSIZES);
	return (unsigned)szind;
}

hpa_pool_t *hpa_pool_lookup(hpa_pool_set_t *set, unsigned key);

/* Selects a pool-local slot and initializes its shard if necessary. */
hpa_shard_t *hpa_pool_pick_shard(
    tsdn_t *tsdn, hpa_pool_t *pool, unsigned hint);

void hpa_pools_prefork(tsdn_t *tsdn, hpa_pool_set_t *set);
void hpa_pools_postfork_parent(tsdn_t *tsdn, hpa_pool_set_t *set);
void hpa_pools_postfork_child(tsdn_t *tsdn, hpa_pool_set_t *set);

/* The two composed: the whole of the allocation-side routing decision. */
JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_route(tsdn_t *tsdn, hpa_pool_set_t *set, size_t size, bool slab,
    szind_t szind, unsigned hint) {
	hpa_pool_t *pool = hpa_pool_lookup(set, hpa_route_key(size, slab,
	    szind));
	assert(pool != NULL);
	return hpa_pool_pick_shard(tsdn, pool, hint);
}

#endif /* JEMALLOC_INTERNAL_HPA_POOL_H */
