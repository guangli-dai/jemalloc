#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/deferral.h"
#include "jemalloc/internal/hpa_pool.h"
#include "jemalloc/internal/malloc_io.h"

const char *const hpa_pool_pick_type_names[] = {"arena", "roundrobin"};

hpa_pool_set_t hpa_pools_global;

bool
hpa_pool_configs_identity(hpa_pool_configs_t *configs, unsigned narenas) {
	assert(narenas > 0);
	memset(configs, 0, sizeof(*configs));
	if (narenas > HPA_MAX_SHARDS_PER_POOL) {
		malloc_printf("<jemalloc>: hpa pools: %u arenas do not fit in "
		    "the identity pool's %u shard slots\n", narenas,
		    (unsigned)HPA_MAX_SHARDS_PER_POOL);
		return true;
	}
	configs->npools = 1;
	configs->pools[0].size_max = HUGEPAGE;
	configs->pools[0].nshards = narenas;
	configs->pick = hpa_pool_pick_arena;
	return false;
}

/*
 * Returns true (having complained) if the configuration cannot be used.
 * Clamping of the total shard count is not an error -- it is reported and the
 * configuration is scaled down -- but one that does not describe a total
 * function over [1, HUGEPAGE] is, because the router would then have sizes
 * with no pool and pa_alloc() would silently fall through to the PAC.
 */
bool
hpa_pool_configs_validate(const hpa_pool_configs_t *configs) {
	if (configs->npools == 0 || configs->npools > HPA_MAX_POOLS) {
		malloc_printf("<jemalloc>: hpa pools: npools %u out of range "
		    "(1..%u)\n", configs->npools, (unsigned)HPA_MAX_POOLS);
		return true;
	}
	if (configs->pick >= hpa_pool_pick_limit) {
		malloc_printf("<jemalloc>: hpa pools: bad picker %u\n",
		    (unsigned)configs->pick);
		return true;
	}

	size_t prev_max = 0;
	for (unsigned i = 0; i < configs->npools; i++) {
		size_t size_max = configs->pools[i].size_max;
		if (configs->pools[i].nshards == 0) {
			malloc_printf("<jemalloc>: hpa pools: pool %u has no "
			    "shards\n", i);
			return true;
		}
		/*
		 * Bound each pool before anything sums them.  The clamp below
		 * would scale an over-large request down, but only if the sum
		 * is computed without wrapping; rejecting nonsense here means
		 * the arithmetic downstream cannot be fed a value that makes
		 * it lie.
		 */
		if (configs->pools[i].nshards > HPA_MAX_SHARDS_PER_POOL) {
			malloc_printf("<jemalloc>: hpa pools: pool %u asks for "
			    "%u shards, more than the %u slots "
			    "allowed\n", i,
			    configs->pools[i].nshards,
			    (unsigned)HPA_MAX_SHARDS_PER_POOL);
			return true;
		}
		if (size_max <= prev_max) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound "
			    "%zu is not above the previous bound %zu\n", i,
			    size_max, prev_max);
			return true;
		}
		if (size_max > HUGEPAGE) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound "
			    "%zu exceeds HUGEPAGE %zu\n", i, size_max,
			    (size_t)HUGEPAGE);
			return true;
		}
		if (sz_s2u(size_max) != size_max) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound %zu "
			    "is not an allocation size class\n", i, size_max);
			return true;
		}
		prev_max = size_max;
	}
	/* Totality over every requested allocation size routed by the HPA. */
	if (prev_max != HUGEPAGE) {
		malloc_printf("<jemalloc>: hpa pools: last pool bound %zu does "
		    "not reach HUGEPAGE %zu; sizes above it would have no "
		    "pool\n", prev_max, (size_t)HUGEPAGE);
		return true;
	}
	return false;
}

/*
 * Total shards is bounded by HPA_MAX_SHARDS_TOTAL.  Scale the configuration
 * proportionally rather than refusing to boot, but say so: a
 * silently smaller fan-out than asked for is the kind of thing that gets
 * mistaken for a tuning result.
 */
JET_EXTERN void
hpa_pool_configs_clamp(hpa_pool_configs_t *configs) {
	/*
	 * 64-bit: validate() has already bounded each pool, but the sum of up
	 * to HPA_MAX_POOLS of them still must not wrap, or an over-large
	 * request could masquerade as a small one and skip clamping entirely.
	 */
	uint64_t requested = 0;
	for (unsigned i = 0; i < configs->npools; i++) {
		requested += configs->pools[i].nshards;
	}
	if (requested <= HPA_MAX_SHARDS_TOTAL) {
		return;
	}

	/*
	 * Scale proportionally, but allocate against a running budget rather
	 * than rounding each share independently.  Independent rounding does
	 * not work: flooring each share and then lifting the zeroes to one can
	 * overshoot, because each lift adds nearly a whole shard that the
	 * floors did not save.
	 *
	 * Reserving one shard for each pool still to come keeps every pool
	 * non-empty while making the total impossible to exceed.
	 */
	unsigned remaining = HPA_MAX_SHARDS_TOTAL;
	unsigned total = 0;
	for (unsigned i = 0; i < configs->npools; i++) {
		unsigned pools_after = configs->npools - i - 1;
		assert(remaining > pools_after);
		unsigned budget = remaining - pools_after;
		unsigned scaled = (unsigned)(((uint64_t)configs->pools[i].nshards
		    * HPA_MAX_SHARDS_TOTAL) / requested);
		if (scaled == 0) {
			scaled = 1;
		}
		if (scaled > budget) {
			scaled = budget;
		}
		configs->pools[i].nshards = scaled;
		remaining -= scaled;
		total += scaled;
	}
	assert(total <= HPA_MAX_SHARDS_TOTAL);

	malloc_printf("<jemalloc>: hpa pools: requested %" FMTu64 " shards, "
	    "clamped to %u (limit %u)\n",
	    requested, total, (unsigned)HPA_MAX_SHARDS_TOTAL);
}

/* Returns the first pool whose requested-size-class bound covers key. */
hpa_pool_t *
hpa_pool_lookup(hpa_pool_set_t *set, unsigned key) {
	assert(set->initialized);
	assert(key < HPA_ROUTE_NKEYS);
	for (unsigned i = 0; i < set->npools; i++) {
		if (key <= (unsigned)set->pools[i].szind_max) {
			return &set->pools[i];
		}
	}
	not_reached();
}

bool
hpa_pools_boot(tsdn_t *tsdn, hpa_pool_set_t *set, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_pool_configs_t *configs, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts) {
	assert(!set->initialized);
	assert(hpa_supported());
	/* The routing key covers every requested size accepted by the HPA. */
	assert(sz_size2index(HUGEPAGE) < HPA_ROUTE_NKEYS);

	hpa_pool_configs_t local = *configs;
	if (hpa_pool_configs_validate(&local)) {
		return true;
	}
	hpa_pool_configs_clamp(&local);

	memset(set, 0, sizeof(*set));
	set->npools = local.npools;
	set->base = base;
	set->central = central;
	set->emap = emap;
	set->edata_cache = edata_cache;
	set->opts = *opts;
	set->sec_opts = *sec_opts;

	for (unsigned i = 0; i < local.npools; i++) {
		hpa_pool_t *pool = &set->pools[i];
		pool->size_max = local.pools[i].size_max;
		pool->szind_max = sz_size2index(pool->size_max);
		pool->pool_ind = i;
		pool->set = set;
		pool->nshards = local.pools[i].nshards;
		pool->pick = local.pick;
		if (malloc_mutex_init(&pool->init_mtx, "hpa_pool_init",
		    WITNESS_RANK_HPA_POOL, malloc_mutex_rank_exclusive)) {
			return true;
		}
		atomic_store_u(&pool->rr_next, 0, ATOMIC_RELAXED);
		for (unsigned j = 0; j < HPA_MAX_SHARDS_PER_POOL; j++) {
			atomic_store_p(&pool->shards[j], NULL, ATOMIC_RELAXED);
		}
	}

	set->initialized = true;
	return false;
}

hpa_shard_t *
hpa_pool_pick_shard(tsdn_t *tsdn, hpa_pool_t *pool, unsigned hint) {
	assert(pool->nshards > 0);

	unsigned idx;
	switch (pool->pick) {
	case hpa_pool_pick_arena:
		idx = hint % pool->nshards;
		break;
	case hpa_pool_pick_roundrobin:
		idx = atomic_fetch_add_u(&pool->rr_next, 1, ATOMIC_RELAXED)
		    % pool->nshards;
		break;
	default:
		not_reached();
	}

	assert(idx < pool->nshards);
	hpa_shard_t *shard = (hpa_shard_t *)atomic_load_p(
	    &pool->shards[idx], ATOMIC_ACQUIRE);
	if (shard != NULL) {
		return shard;
	}

	malloc_mutex_lock(tsdn, &pool->init_mtx);
	shard = (hpa_shard_t *)atomic_load_p(&pool->shards[idx],
	    ATOMIC_RELAXED);
	if (shard == NULL) {
		hpa_pool_set_t *set = pool->set;
		shard = (hpa_shard_t *)base_alloc(tsdn, set->base,
		    sizeof(hpa_shard_t), CACHELINE);
		unsigned id = hpa_pool_shard_id(pool, idx);
		if (shard == NULL || hpa_shard_init(tsdn, shard, set->central,
		        set->emap, set->base, set->edata_cache, id, &set->opts,
		        &set->sec_opts)) {
			shard = NULL;
		} else {
			atomic_store_p(&pool->shards[idx], shard, ATOMIC_RELEASE);
		}
	}
	malloc_mutex_unlock(tsdn, &pool->init_mtx);
	return shard;
}

typedef void (*hpa_pool_shard_cb_t)(tsdn_t *, hpa_shard_t *);

static void
hpa_pools_for_each_shard(tsdn_t *tsdn, hpa_pool_shard_cb_t cb) {
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard != NULL) {
				cb(tsdn, shard);
			}
		}
	}
}

void
hpa_pools_prefork2(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	/* Freeze the set of initialized slots before locking their shards. */
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		malloc_mutex_prefork(tsdn,
		    &hpa_pools_global.pools[i].init_mtx);
	}
	hpa_pools_for_each_shard(tsdn, hpa_shard_prefork2);
}

void
hpa_pools_prefork3(tsdn_t *tsdn) {
	if (hpa_pools_ready()) {
		hpa_pools_for_each_shard(tsdn, hpa_shard_prefork3);
	}
}

void
hpa_pools_prefork4(tsdn_t *tsdn) {
	if (hpa_pools_ready()) {
		hpa_pools_for_each_shard(tsdn, hpa_shard_prefork4);
	}
}

void
hpa_pools_prefork5(tsdn_t *tsdn) {
	if (hpa_pools_ready()) {
		edata_cache_prefork(tsdn, hpa_pools_global.edata_cache);
	}
}

void
hpa_pools_postfork_parent(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	edata_cache_postfork_parent(tsdn, hpa_pools_global.edata_cache);
	hpa_pools_for_each_shard(tsdn, hpa_shard_postfork_parent);
	for (unsigned i = hpa_pools_global.npools; i-- > 0;) {
		malloc_mutex_postfork_parent(tsdn,
		    &hpa_pools_global.pools[i].init_mtx);
	}
}

void
hpa_pools_postfork_child(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	edata_cache_postfork_child(tsdn, hpa_pools_global.edata_cache);
	hpa_pools_for_each_shard(tsdn, hpa_shard_postfork_child);
	for (unsigned i = hpa_pools_global.npools; i-- > 0;) {
		malloc_mutex_postfork_child(tsdn,
		    &hpa_pools_global.pools[i].init_mtx);
	}
}

static bool hpa_pools_deferral_allowed;

static void
hpa_pool_enable_deferral(tsdn_t *tsdn, hpa_shard_t *shard) {
	hpa_shard_set_deferral_allowed(tsdn, shard, hpa_pools_deferral_allowed);
}

void
hpa_pools_set_deferral_allowed(tsdn_t *tsdn, bool allowed) {
	hpa_pools_deferral_allowed = allowed;
	hpa_pools_global.opts.deferral_allowed = allowed;
	if (hpa_pools_ready()) {
		hpa_pools_for_each_shard(tsdn, hpa_pool_enable_deferral);
	}
}

void
hpa_pools_do_deferred_work(tsdn_t *tsdn) {
	if (hpa_pools_ready()) {
		hpa_pools_for_each_shard(tsdn, hpa_shard_do_deferred_work);
	}
}

uint64_t
hpa_pools_time_until_deferred_work(tsdn_t *tsdn) {
	uint64_t time = DEFERRED_WORK_MAX;
	if (!hpa_pools_ready()) {
		return time;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard != NULL) {
				uint64_t candidate = hpa_time_until_deferred_work(
				    tsdn, shard);
				if (candidate < time) {
					time = candidate;
				}
			}
		}
	}
	return time;
}

void
hpa_pools_stats_merge(tsdn_t *tsdn, hpa_shard_stats_t *dst) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard != NULL) {
				hpa_shard_stats_merge(tsdn, shard, dst);
			}
		}
	}
}

size_t
hpa_pools_ndirty(void) {
	size_t ndirty = 0;
	if (!hpa_pools_ready()) {
		return ndirty;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard != NULL) {
				ndirty += psset_ndirty(&shard->psset);
			}
		}
	}
	return ndirty;
}

void
hpa_pools_flush(tsdn_t *tsdn) {
	if (hpa_pools_ready()) {
		hpa_pools_for_each_shard(tsdn, hpa_shard_flush);
	}
}

void
hpa_pools_purge(tsdn_t *tsdn) {
	hpa_pools_flush(tsdn);
	hpa_pools_do_deferred_work(tsdn);
}
