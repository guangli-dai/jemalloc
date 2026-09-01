#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/deferral.h"
#include "jemalloc/internal/hpa_pool.h"
#include "jemalloc/internal/malloc_io.h"

const char *const hpa_pool_pick_type_names[] = {"arena", "roundrobin"};
const char *const hpa_pool_opt_names[] = {
#define OP(field) #field,
	HPA_POOL_SHARD_OPTS
#undef OP
#define OP(field) "sec_" #field,
	HPA_POOL_SEC_OPTS
#undef OP
};

hpa_pool_set_t hpa_pools_global;

bool
hpa_pool_configs_identity(hpa_pool_configs_t *configs, unsigned narenas,
    unsigned nshards_max) {
	assert(narenas > 0);
	hpa_pool_configs_init(configs);
	if (nshards_max == 0 || nshards_max > HPA_MAX_SHARDS_PER_POOL) {
		return true;
	}
	configs->nshards_max = nshards_max;
	configs->npools = 1;
	configs->pools[0].size_max = HUGEPAGE;
	configs->pools[0].nshards = narenas < nshards_max
	    ? narenas : nshards_max;
	configs->pick = hpa_pool_pick_arena;
	return false;
}

void
hpa_pool_configs_init(hpa_pool_configs_t *configs) {
	memset(configs, 0, sizeof(*configs));
	configs->pick = hpa_pool_pick_arena;
	configs->nshards_max = HPA_POOL_NSHARDS_MAX_DEFAULT;
}

bool
hpa_pool_configs_add(hpa_pool_configs_t *configs, size_t size_start,
    size_t size_end, unsigned nshards) {
	if (configs->npools == HPA_MAX_POOLS) {
		return true;
	}
	size_t expected = configs->npools == 0 ? 1
	    : configs->pools[configs->npools - 1].size_max + 1;
	if (size_start != expected || size_end < size_start) {
		return true;
	}
	configs->pools[configs->npools].size_max = size_end;
	configs->pools[configs->npools].nshards = nshards;
	configs->npools++;
	return false;
}

static bool
hpa_pool_parse_u64(const char *val, size_t len, uint64_t min, uint64_t max,
    uint64_t *out) {
	char *end;
	uintmax_t parsed = malloc_strtoumax(val, &end, 0);
	if (end != val + len || parsed < min || parsed > max) {
		return true;
	}
	*out = (uint64_t)parsed;
	return false;
}

static bool
hpa_pool_parse_size(const char *val, size_t len, size_t min, size_t max,
    size_t *out) {
	uint64_t parsed;
	if (hpa_pool_parse_u64(val, len, min, max, &parsed)) {
		return true;
	}
	*out = (size_t)parsed;
	return false;
}

static hpa_pool_config_t *
hpa_pool_configs_find(hpa_pool_configs_t *configs, size_t size_start,
    size_t size_end) {
	size_t start = 1;
	for (unsigned i = 0; i < configs->npools; i++) {
		if (start == size_start && configs->pools[i].size_max == size_end) {
			return &configs->pools[i];
		}
		start = configs->pools[i].size_max + 1;
	}
	return NULL;
}

bool
hpa_pool_configs_set_opt(hpa_pool_configs_t *configs, size_t size_start,
    size_t size_end, const char *key, size_t keylen, const char *val,
    size_t vallen) {
	hpa_pool_config_t *pool = hpa_pool_configs_find(configs, size_start,
	    size_end);
	if (pool == NULL) {
		return true;
	}
	unsigned which = hpa_pool_opt_limit;
	for (unsigned i = 0; i < hpa_pool_opt_limit; i++) {
		if (strlen(hpa_pool_opt_names[i]) == keylen
		    && strncmp(hpa_pool_opt_names[i], key, keylen) == 0) {
			which = i;
			break;
		}
	}
	bool err = false;
	switch (which) {
	case hpa_pool_opt_slab_max_alloc:
		err = hpa_pool_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.slab_max_alloc);
		break;
	case hpa_pool_opt_hugification_threshold:
		err = hpa_pool_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.hugification_threshold);
		break;
	case hpa_pool_opt_dirty_mult:
		if (vallen == 2 && strncmp(val, "-1", 2) == 0) {
			pool->opts.dirty_mult = (fxp_t)-1;
		} else {
			char *end;
			err = fxp_parse(&pool->opts.dirty_mult, val, &end)
			    || (size_t)(end - val) != vallen;
		}
		break;
	case hpa_pool_opt_hugify_delay_ms:
		err = hpa_pool_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.hugify_delay_ms);
		break;
	case hpa_pool_opt_hugify_sync:
		if (vallen == 4 && strncmp(val, "true", 4) == 0) {
			pool->opts.hugify_sync = true;
		} else if (vallen == 5 && strncmp(val, "false", 5) == 0) {
			pool->opts.hugify_sync = false;
		} else {
			err = true;
		}
		break;
	case hpa_pool_opt_min_purge_interval_ms:
		err = hpa_pool_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.min_purge_interval_ms);
		break;
	case hpa_pool_opt_purge_threshold:
		err = hpa_pool_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.purge_threshold);
		break;
	case hpa_pool_opt_min_purge_delay_ms:
		err = hpa_pool_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.min_purge_delay_ms);
		break;
	case hpa_pool_opt_hugify_style:
		err = true;
		for (unsigned i = 0; i < hpa_hugify_style_limit; i++) {
			const char *name = hpa_hugify_style_names[i];
			if (strlen(name) == vallen
			    && strncmp(name, val, vallen) == 0) {
				pool->opts.hugify_style = (hpa_hugify_style_t)i;
				err = false;
				break;
			}
		}
		break;
	case hpa_pool_opt_sec_nshards:
		err = hpa_pool_parse_size(val, vallen, 0, 255,
		    &pool->sec_opts.nshards);
		break;
	case hpa_pool_opt_sec_max_alloc:
		err = hpa_pool_parse_size(val, vallen, PAGE,
		    USIZE_GROW_SLOW_THRESHOLD, &pool->sec_opts.max_alloc);
		break;
	case hpa_pool_opt_sec_max_bytes:
		err = hpa_pool_parse_size(val, vallen,
		    SEC_OPTS_MAX_BYTES_DEFAULT, SIZE_T_MAX,
		    &pool->sec_opts.max_bytes);
		break;
	default:
		return true;
	}
	if (err) {
		return true;
	}
	pool->opts_set |= (uint32_t)1 << which;
	return false;
}

static void
hpa_pool_opts_apply(hpa_shard_opts_t *opts, sec_opts_t *sec_opts,
    const hpa_pool_config_t *config) {
	uint32_t set = config->opts_set;
#define OP(field)                                                              \
	if ((set & ((uint32_t)1 << hpa_pool_opt_##field)) != 0) {              \
		opts->field = config->opts.field;                                \
	}
	HPA_POOL_SHARD_OPTS
#undef OP
#define OP(field)                                                              \
	if ((set & ((uint32_t)1 << hpa_pool_opt_sec_##field)) != 0) {          \
		sec_opts->field = config->sec_opts.field;                        \
	}
	HPA_POOL_SEC_OPTS
#undef OP
	assert((set & ~(((uint32_t)1 << hpa_pool_opt_limit) - 1)) == 0);
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
	if (configs->nshards_max < configs->npools
	    || configs->nshards_max > HPA_MAX_SHARDS_TOTAL) {
		malloc_printf("<jemalloc>: hpa pools: nshards_max %u out of "
		    "range (%u..%u)\n", configs->nshards_max,
		    configs->npools, (unsigned)HPA_MAX_SHARDS_TOTAL);
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
	unsigned limit = configs->nshards_max;
	if (requested <= limit) {
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
	unsigned remaining = limit;
	unsigned total = 0;
	for (unsigned i = 0; i < configs->npools; i++) {
		unsigned pools_after = configs->npools - i - 1;
		assert(remaining > pools_after);
		unsigned budget = remaining - pools_after;
		unsigned scaled = (unsigned)(((uint64_t)configs->pools[i].nshards
		    * limit) / requested);
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
	assert(total <= limit);

	malloc_printf("<jemalloc>: hpa pools: requested %" FMTu64 " shards, "
	    "clamped to %u (limit %u)\n",
	    requested, total, limit);
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

	size_t size_min = 1;
	for (unsigned i = 0; i < local.npools; i++) {
		hpa_pool_t *pool = &set->pools[i];
		pool->size_min = size_min;
		pool->size_max = local.pools[i].size_max;
		pool->szind_max = sz_size2index(pool->size_max);
		pool->pool_ind = i;
		pool->set = set;
		pool->nshards = local.pools[i].nshards;
		pool->pick = local.pick;
		pool->opts = *opts;
		pool->sec_opts = *sec_opts;
		hpa_pool_opts_apply(&pool->opts, &pool->sec_opts,
		    &local.pools[i]);
		if (malloc_mutex_init(&pool->init_mtx, "hpa_pool_init",
		    WITNESS_RANK_HPA_POOL, malloc_mutex_rank_exclusive)) {
			return true;
		}
		atomic_store_u(&pool->rr_next, 0, ATOMIC_RELAXED);
		for (unsigned j = 0; j < HPA_MAX_SHARDS_PER_POOL; j++) {
			atomic_store_p(&pool->shards[j], NULL, ATOMIC_RELAXED);
		}
		size_min = pool->size_max + 1;
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
		        set->emap, set->base, set->edata_cache, id, &pool->opts,
		        &pool->sec_opts)) {
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
		for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
			hpa_pools_global.pools[i].opts.deferral_allowed = allowed;
		}
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
hpa_pools_stats_merge(tsdn_t *tsdn, hpa_shard_stats_t *dst,
    hpa_pool_stats_t *pool_dst, unsigned npools) {
	if (pool_dst != NULL) {
		memset(pool_dst, 0, npools * sizeof(*pool_dst));
	}
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		hpa_pool_stats_t *out = pool_dst != NULL && i < npools
		    ? &pool_dst[i] : NULL;
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard != NULL) {
				hpa_shard_stats_t stats;
				memset(&stats, 0, sizeof(stats));
				hpa_shard_stats_merge(tsdn, shard, &stats);
				hpa_shard_stats_accum(dst, &stats);
				if (out == NULL) {
					continue;
				}
				malloc_mutex_lock(tsdn, &shard->grow_mtx);
				malloc_mutex_prof_accum(tsdn,
				    &out->mutexes[hpa_pool_mutex_shard_grow],
				    &shard->grow_mtx);
				malloc_mutex_unlock(tsdn, &shard->grow_mtx);
				malloc_mutex_lock(tsdn, &shard->mtx);
				malloc_mutex_prof_accum(tsdn,
				    &out->mutexes[hpa_pool_mutex_shard], &shard->mtx);
				malloc_mutex_unlock(tsdn, &shard->mtx);
				sec_mutex_stats_read(tsdn, &shard->sec,
				    &out->mutexes[hpa_pool_mutex_sec]);
				psset_stats_t *ps = &stats.psset_stats;
				out->npageslabs_nonhuge += ps->slabs[0].npageslabs;
				out->npageslabs_huge += ps->slabs[1].npageslabs;
				out->nactive_nonhuge += ps->slabs[0].nactive;
				out->nactive_huge += ps->slabs[1].nactive;
				out->ndirty_nonhuge += ps->slabs[0].ndirty;
				out->ndirty_huge += ps->slabs[1].ndirty;
				out->npurge_passes +=
				    stats.nonderived_stats.npurge_passes;
				out->npurges += stats.nonderived_stats.npurges;
				out->nhugifies += stats.nonderived_stats.nhugifies;
				out->nhugify_failures +=
				    stats.nonderived_stats.nhugify_failures;
				out->ndehugifies += stats.nonderived_stats.ndehugifies;
			}
		}
	}
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

void
hpa_pools_mtx_prof_reset(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t *pool = &hpa_pools_global.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard = atomic_load_p(&pool->shards[j],
			    ATOMIC_ACQUIRE);
			if (shard == NULL) {
				continue;
			}
			malloc_mutex_lock(tsdn, &shard->grow_mtx);
			malloc_mutex_prof_data_reset(tsdn, &shard->grow_mtx);
			malloc_mutex_unlock(tsdn, &shard->grow_mtx);
			malloc_mutex_lock(tsdn, &shard->mtx);
			malloc_mutex_prof_data_reset(tsdn, &shard->mtx);
			malloc_mutex_unlock(tsdn, &shard->mtx);
			sec_mutex_prof_reset(tsdn, &shard->sec);
		}
	}
}
