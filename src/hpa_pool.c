#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/hpa_pool.h"
#include "jemalloc/internal/malloc_io.h"

const char *const hpa_pool_pick_names[] = {"arena", "roundrobin"};

void
hpa_pool_layout_identity(hpa_pool_layout_t *layout, unsigned narenas) {
	assert(narenas > 0);
	memset(layout, 0, sizeof(*layout));
	layout->npools = 1;
	layout->pools[0].size_max = HUGEPAGE;
	layout->pools[0].nshards = narenas;
	layout->pick = hpa_pool_pick_arena;
}

/*
 * Returns true (having complained) if the layout could not be used as given.
 * Clamping of the total shard count is not an error -- it is reported and the
 * layout is scaled down -- but a layout that does not describe a total
 * function over [PAGE, HUGEPAGE] is, because the router would then have sizes
 * with no pool and pa_alloc() would silently fall through to the PAC.
 */
static bool
hpa_pool_layout_validate(const hpa_pool_layout_t *layout) {
	if (layout->npools == 0 || layout->npools > HPA_MAX_POOLS) {
		malloc_printf("<jemalloc>: hpa pools: npools %u out of range "
		    "(1..%u)\n", layout->npools, (unsigned)HPA_MAX_POOLS);
		return true;
	}
	if (layout->pick >= hpa_pool_pick_limit) {
		malloc_printf("<jemalloc>: hpa pools: bad picker %u\n",
		    (unsigned)layout->pick);
		return true;
	}

	size_t prev_max = 0;
	for (unsigned i = 0; i < layout->npools; i++) {
		size_t size_max = layout->pools[i].size_max;
		if (layout->pools[i].nshards == 0) {
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
		if (layout->pools[i].nshards > HPA_MAX_SHARDS_TOTAL) {
			malloc_printf("<jemalloc>: hpa pools: pool %u asks for "
			    "%u shards, more than the %u total the extent "
			    "owner field can address\n", i,
			    layout->pools[i].nshards,
			    (unsigned)HPA_MAX_SHARDS_TOTAL);
			return true;
		}
		if (size_max <= prev_max) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound "
			    "%zu is not above the previous bound %zu\n", i,
			    size_max, prev_max);
			return true;
		}
		if ((size_max & PAGE_MASK) != 0) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound "
			    "%zu is not page-aligned\n", i, size_max);
			return true;
		}
		if (size_max > HUGEPAGE) {
			malloc_printf("<jemalloc>: hpa pools: pool %u bound "
			    "%zu exceeds HUGEPAGE %zu\n", i, size_max,
			    (size_t)HUGEPAGE);
			return true;
		}
		prev_max = size_max;
	}
	/* Totality: the last band has to reach the largest servable extent. */
	if (prev_max != HUGEPAGE) {
		malloc_printf("<jemalloc>: hpa pools: last pool bound %zu does "
		    "not reach HUGEPAGE %zu; sizes above it would have no "
		    "pool\n", prev_max, (size_t)HUGEPAGE);
		return true;
	}
	return false;
}

/*
 * Total shards is bounded by what fits in an extent's owner field.  Scale the
 * layout down proportionally rather than refusing to boot, but say so: a
 * silently smaller fan-out than asked for is the kind of thing that gets
 * mistaken for a tuning result.
 */
JET_EXTERN void
hpa_pool_layout_clamp(hpa_pool_layout_t *layout) {
	/*
	 * 64-bit: validate() has already bounded each pool, but the sum of up
	 * to HPA_MAX_POOLS of them still must not wrap, or an over-large
	 * request could masquerade as a small one and skip clamping entirely.
	 */
	uint64_t requested = 0;
	for (unsigned i = 0; i < layout->npools; i++) {
		requested += layout->pools[i].nshards;
	}
	if (requested <= HPA_MAX_SHARDS_TOTAL) {
		return;
	}

	/*
	 * Scale proportionally, but allocate against a running budget rather
	 * than rounding each share independently.  Independent rounding does
	 * not work: flooring each share and then lifting the zeroes to one can
	 * overshoot, because each lift adds nearly a whole shard that the
	 * floors did not save.  One huge pool beside seven tiny ones lands on
	 * 4095 + 7 = 4102, past a limit that exists because the id has to fit
	 * in the extent's owner field -- in a release build that truncates and
	 * routes frees to the wrong shard.
	 *
	 * Reserving one shard for each pool still to come keeps every pool
	 * non-empty while making the total impossible to exceed.
	 */
	unsigned remaining = HPA_MAX_SHARDS_TOTAL;
	unsigned total = 0;
	for (unsigned i = 0; i < layout->npools; i++) {
		unsigned pools_after = layout->npools - i - 1;
		assert(remaining > pools_after);
		unsigned budget = remaining - pools_after;
		unsigned scaled = (unsigned)(((uint64_t)layout->pools[i].nshards
		    * HPA_MAX_SHARDS_TOTAL) / requested);
		if (scaled == 0) {
			scaled = 1;
		}
		if (scaled > budget) {
			scaled = budget;
		}
		layout->pools[i].nshards = scaled;
		remaining -= scaled;
		total += scaled;
	}
	assert(total <= HPA_MAX_SHARDS_TOTAL);

	malloc_printf("<jemalloc>: hpa pools: requested %" FMTu64 " shards, "
	    "clamped to %u (limit %u, set by the extent owner field width)\n",
	    requested, total, (unsigned)HPA_MAX_SHARDS_TOTAL);
}

/*
 * Fill the size -> pool table.  A page-size class belongs to the first pool
 * whose band reaches it.
 *
 * Routing granularity is therefore the page-size class, not the byte: a
 * boundary that falls strictly inside a class effectively rounds up to the
 * end of it.  Boundaries that are themselves page-size classes -- which the
 * interesting ones (16 KiB, 64 KiB, 1 MiB) all are -- are exact.
 */
JET_EXTERN void
hpa_pool_build_route_table(hpa_pool_set_t *set) {
	for (unsigned key = 0; key < HPA_ROUTE_NKEYS; key++) {
		size_t      size = sz_pind2sz(key);
		hpa_pool_t *found = NULL;
		for (unsigned i = 0; i < set->npools; i++) {
			if (size <= set->pools[i].size_max) {
				found = &set->pools[i];
				break;
			}
		}
		/*
		 * Keys past HUGEPAGE describe extents the HPA will never be
		 * asked for; hpa_route_key() asserts size <= HUGEPAGE.  Pin
		 * them to the last pool anyway so the table has no NULLs to
		 * trip over.
		 */
		if (found == NULL) {
			found = &set->pools[set->npools - 1];
		}
		set->pool_by_key[key] = found;
	}
}

bool
hpa_pools_boot(tsdn_t *tsdn, hpa_pool_set_t *set, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_pool_layout_t *layout, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts) {
	assert(!set->initialized);
	assert(hpa_supported());
	/*
	 * The routing table has to span every size the HPA can serve.  This is
	 * inherited from the psset's own bound, so it should hold by
	 * construction; check it rather than assume it, because a page-size
	 * configuration that broke it would produce silent misrouting.
	 */
	assert(sz_psz2ind(HUGEPAGE) < HPA_ROUTE_NKEYS);

	hpa_pool_layout_t local = *layout;
	if (hpa_pool_layout_validate(&local)) {
		return true;
	}
	hpa_pool_layout_clamp(&local);

	memset(set, 0, sizeof(*set));
	set->npools = local.npools;
	/*
	 * Remember the shared cache.  Every shard draws edata_t from it, and
	 * it is a mutex as well as a lifetime fix: fork has to lock it exactly
	 * once, after all the shard mutexes.  Holding it here keeps that the
	 * pool set's business rather than something the fork code has to be
	 * told separately.
	 */
	set->edata_cache = edata_cache;

	unsigned nshards_total = 0;
	for (unsigned i = 0; i < local.npools; i++) {
		nshards_total += local.pools[i].nshards;
	}
	set->nshards_total = nshards_total;

	set->shards = (hpa_shard_t *)base_alloc(tsdn, base,
	    nshards_total * sizeof(hpa_shard_t), CACHELINE);
	if (set->shards == NULL) {
		memset(set, 0, sizeof(*set));
		return true;
	}

	size_t   size_min = PAGE;
	unsigned next_shard = 0;
	for (unsigned i = 0; i < local.npools; i++) {
		hpa_pool_t *pool = &set->pools[i];
		pool->size_min = size_min;
		pool->size_max = local.pools[i].size_max;
		pool->opts = *opts;
		pool->sec_opts = *sec_opts;
		pool->first_shard = next_shard;
		pool->nshards = local.pools[i].nshards;
		pool->pick = local.pick;
		atomic_store_u(&pool->rr_next, 0, ATOMIC_RELAXED);

		for (unsigned j = 0; j < pool->nshards; j++) {
			unsigned id = pool->first_shard + j;
			if (hpa_shard_init(tsdn, &set->shards[id], central,
			        emap, base, edata_cache, id, &pool->opts,
			        &pool->sec_opts)) {
				/*
				 * Leave nothing half-built behind.  The base
				 * allocation and any shards already brought up
				 * are unreachable from here on, which is the
				 * same as every other boot-time base
				 * allocation; what matters is that no caller
				 * can observe a partially populated set and
				 * mistake it for a usable one.
				 */
				memset(set, 0, sizeof(*set));
				return true;
			}
		}

		size_min = pool->size_max + 1;
		next_shard += pool->nshards;
	}
	assert(next_shard == nshards_total);

	hpa_pool_build_route_table(set);
	set->initialized = true;
	return false;
}

hpa_shard_t *
hpa_pool_pick_shard(hpa_pool_set_t *set, hpa_pool_t *pool, unsigned hint) {
	assert(set->initialized);
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
	unsigned id = pool->first_shard + idx;
	assert(id < set->nshards_total);
	return &set->shards[id];
}
