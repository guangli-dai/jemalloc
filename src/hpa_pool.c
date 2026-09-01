#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/deferral.h"
#include "jemalloc/internal/hpa_pool.h"
#include "jemalloc/internal/malloc_io.h"

const char *const hpa_pool_pick_names[] = {"arena", "roundrobin"};

const char *const hpa_pool_opt_names[] = {
#define OP(field) #field,
    HPA_POOL_SHARD_OPTS
#undef OP
#define OP(field) "sec_" #field,
	HPA_POOL_SEC_OPTS
#undef OP
};

void
hpa_pool_layout_identity(hpa_pool_layout_t *layout, unsigned narenas,
    unsigned nshards_max) {
	assert(narenas > 0);
	assert(nshards_max > 0);
	hpa_pool_layout_init(layout);
	layout->nshards_max = nshards_max;
	layout->npools = 1;
	layout->pools[0].size_max = HUGEPAGE;
	/*
	 * Apply the cap here rather than leaving it to the clamp.  narenas is
	 * not a request for shards -- on a large machine it is 705 by
	 * accident -- so scaling it down is ordinary, and the clamp announces
	 * itself on stderr.  A default configuration should not warn about a
	 * number nobody chose.
	 */
	layout->pools[0].nshards = narenas < nshards_max ? narenas
	                                                 : nshards_max;
}

void
hpa_pool_layout_init(hpa_pool_layout_t *layout) {
	memset(layout, 0, sizeof(*layout));
	layout->pick = hpa_pool_pick_arena;
	layout->nshards_max = HPA_POOL_NSHARDS_MAX_DEFAULT;
}

bool
hpa_pool_layout_add(hpa_pool_layout_t *layout, size_t size_start,
    size_t size_end, unsigned nshards) {
	if (layout->npools == HPA_MAX_POOLS) {
		return true;
	}
	/*
	 * Bands must tile [PAGE, HUGEPAGE] exactly.  Checking abutment here
	 * catches a gap at the point it is written; hpa_pools_boot() re-checks
	 * the whole layout, since it also has to reject one that never reaches
	 * HUGEPAGE.
	 */
	size_t expect_start = (layout->npools == 0)
	    ? PAGE
	    : layout->pools[layout->npools - 1].size_max + 1;
	if (size_start != expect_start || size_end < size_start) {
		return true;
	}
	layout->pools[layout->npools].size_max = size_end;
	layout->pools[layout->npools].nshards = nshards;
	layout->npools++;
	return false;
}

/*
 * Per-pool option parsing.
 *
 * Values are parsed here rather than in the configuration reader so that the
 * name, the range check and the field being written stay together.  Each range
 * mirrors the corresponding global option's, so that "the same value that is
 * legal globally is legal per pool" holds without the two drifting apart.
 */

static bool
hpa_pool_opt_parse_u64(const char *val, size_t vallen, uint64_t min,
    uint64_t max, uint64_t *result) {
	char      *end;
	uintmax_t  parsed = malloc_strtoumax(val, &end, 0);
	if (end != val + vallen || parsed < min || parsed > max) {
		return true;
	}
	*result = (uint64_t)parsed;
	return false;
}

static bool
hpa_pool_opt_parse_size(const char *val, size_t vallen, size_t min, size_t max,
    size_t *result) {
	uint64_t parsed;
	if (hpa_pool_opt_parse_u64(val, vallen, min, max, &parsed)) {
		return true;
	}
	*result = (size_t)parsed;
	return false;
}

static bool
hpa_pool_opt_parse_bool(const char *val, size_t vallen, bool *result) {
	if (vallen == sizeof("true") - 1 && strncmp(val, "true", vallen) == 0) {
		*result = true;
		return false;
	}
	if (vallen == sizeof("false") - 1
	    && strncmp(val, "false", vallen) == 0) {
		*result = false;
		return false;
	}
	return true;
}

/*
 * The band a size range names, or NULL.  Bands are addressed by their range
 * rather than their index so that reordering hpa_pools cannot silently
 * re-target an override onto a different band.
 */
static hpa_pool_layout_entry_t *
hpa_pool_layout_find(hpa_pool_layout_t *layout, size_t size_start,
    size_t size_end) {
	size_t size_min = PAGE;
	for (unsigned i = 0; i < layout->npools; i++) {
		if (size_min == size_start
		    && layout->pools[i].size_max == size_end) {
			return &layout->pools[i];
		}
		size_min = layout->pools[i].size_max + 1;
	}
	return NULL;
}

bool
hpa_pool_layout_set_opt(hpa_pool_layout_t *layout, size_t size_start,
    size_t size_end, const char *key, size_t keylen, const char *val,
    size_t vallen) {
	hpa_pool_layout_entry_t *pool = hpa_pool_layout_find(layout, size_start,
	    size_end);
	if (pool == NULL) {
		malloc_printf("<jemalloc>: hpa pools: no band %zu-%zu to set "
		    "options on.  Bands must be named exactly as configured, "
		    "and hpa_pools must appear before hpa_pool_opts in "
		    "MALLOC_CONF -- hpa_pools replaces the whole layout, "
		    "including any overrides already recorded.\n", size_start,
		    size_end);
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
		err = hpa_pool_opt_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.slab_max_alloc);
		break;
	case hpa_pool_opt_hugification_threshold:
		err = hpa_pool_opt_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.hugification_threshold);
		break;
	case hpa_pool_opt_dirty_mult: {
		/*
		 * Same spelling as the global hpa_dirty_mult, including the
		 * "-1" that disables purging outright, which fxp cannot
		 * represent as a fraction.
		 */
		if (vallen == 2 && strncmp(val, "-1", 2) == 0) {
			pool->opts.dirty_mult = (fxp_t)-1;
			break;
		}
		fxp_t ratio;
		char *end;
		err = fxp_parse(&ratio, val, &end)
		    || (size_t)(end - val) != vallen;
		if (!err) {
			pool->opts.dirty_mult = ratio;
		}
		break;
	}
	case hpa_pool_opt_hugify_delay_ms:
		err = hpa_pool_opt_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.hugify_delay_ms);
		break;
	case hpa_pool_opt_hugify_sync:
		err = hpa_pool_opt_parse_bool(val, vallen,
		    &pool->opts.hugify_sync);
		break;
	case hpa_pool_opt_min_purge_interval_ms:
		err = hpa_pool_opt_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.min_purge_interval_ms);
		break;
	case hpa_pool_opt_purge_threshold:
		err = hpa_pool_opt_parse_size(val, vallen, PAGE, HUGEPAGE,
		    &pool->opts.purge_threshold);
		break;
	case hpa_pool_opt_min_purge_delay_ms:
		err = hpa_pool_opt_parse_u64(val, vallen, 0, UINT64_MAX,
		    &pool->opts.min_purge_delay_ms);
		break;
	case hpa_pool_opt_hugify_style: {
		err = true;
		for (int i = 0; i < hpa_hugify_style_limit; i++) {
			if (strlen(hpa_hugify_style_names[i]) == vallen
			    && strncmp(hpa_hugify_style_names[i], val, vallen)
			        == 0) {
				pool->opts.hugify_style = (hpa_hugify_style_t)i;
				err = false;
				break;
			}
		}
		break;
	}
	case hpa_pool_opt_sec_nshards:
		err = hpa_pool_opt_parse_size(val, vallen, 0, 255,
		    &pool->sec_opts.nshards);
		break;
	/*
	 * The SEC bounds are not cosmetic: sec_init() asserts both, and a
	 * value the global parser would have clipped becomes a debug-build
	 * abort and a release-build misconfiguration if it arrives this way
	 * instead.  Rejected rather than clipped, because a per-pool override
	 * is a deliberate statement and silently adjusting it is worse than
	 * refusing it.
	 */
	case hpa_pool_opt_sec_max_alloc:
		err = hpa_pool_opt_parse_size(val, vallen, PAGE,
		    USIZE_GROW_SLOW_THRESHOLD, &pool->sec_opts.max_alloc);
		break;
	case hpa_pool_opt_sec_max_bytes:
		err = hpa_pool_opt_parse_size(val, vallen,
		    SEC_OPTS_MAX_BYTES_DEFAULT, SIZE_T_MAX,
		    &pool->sec_opts.max_bytes);
		break;
	default:
		malloc_printf("<jemalloc>: hpa pools: unknown per-pool option "
		    "\"%.*s\"\n", (int)keylen, key);
		return true;
	}

	if (err) {
		malloc_printf("<jemalloc>: hpa pools: bad value \"%.*s\" for "
		    "per-pool option %s\n", (int)vallen, val,
		    hpa_pool_opt_names[which]);
		return true;
	}
	pool->opts_set |= (uint32_t)1 << which;
	return false;
}

/*
 * Fold a band's overrides onto the process-wide defaults it inherits.  Only
 * the fields the configuration actually named are touched, so a global option
 * parsed after the override still reaches every band that did not override it.
 */
static void
hpa_pool_opts_apply(hpa_shard_opts_t *opts, sec_opts_t *sec_opts,
    const hpa_pool_layout_entry_t *entry) {
	uint32_t set = entry->opts_set;
	/*
	 * Generated from the same lists as the enum and the names, so an
	 * option cannot be accepted from MALLOC_CONF and then quietly not
	 * applied -- which is the drift that would be hardest to notice, since
	 * it looks exactly like the option not working.
	 */
#define OP(field)                                                              \
	if ((set & ((uint32_t)1 << hpa_pool_opt_##field)) != 0) {              \
		opts->field = entry->opts.field;                               \
	}
	HPA_POOL_SHARD_OPTS
#undef OP
#define OP(field)                                                              \
	if ((set & ((uint32_t)1 << hpa_pool_opt_sec_##field)) != 0) {          \
		sec_opts->field = entry->sec_opts.field;                       \
	}
	HPA_POOL_SEC_OPTS
#undef OP
	assert((set & ~(((uint32_t)1 << hpa_pool_opt_limit) - 1)) == 0);
}

/*
 * Returns true (having complained) if the layout could not be used as given.
 * Clamping of the total shard count is not an error -- it is reported and the
 * layout is scaled down -- but a layout that does not describe a total
 * function over [PAGE, HUGEPAGE] is, because the router would then have sizes
 * with no pool and pa_alloc() would silently fall through to the PAC.
 */
bool
hpa_pool_layout_validate(const hpa_pool_layout_t *layout) {
	if (layout->npools == 0 || layout->npools > HPA_MAX_POOLS) {
		malloc_printf("<jemalloc>: hpa pools: npools %u out of range "
		    "(1..%u)\n", layout->npools, (unsigned)HPA_MAX_POOLS);
		return true;
	}
	/*
	 * Every pool needs at least one shard, so the total cap has to leave
	 * room for one each.  Rejecting rather than quietly dropping pools:
	 * a pool that vanished would send its size band to whichever pool
	 * inherited the band, which is a routing change the caller did not ask
	 * for and would have no way to notice.
	 */
	if (layout->nshards_max < layout->npools
	    || layout->nshards_max > HPA_MAX_SHARDS_TOTAL) {
		malloc_printf("<jemalloc>: hpa pools: nshards_max %u out of "
		    "range (%u..%u); it must leave at least one shard per "
		    "pool\n", layout->nshards_max, layout->npools,
		    (unsigned)HPA_MAX_SHARDS_TOTAL);
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
 * Total shards is bounded twice: by layout->nshards_max, which is a tuning
 * choice, and by HPA_MAX_SHARDS_TOTAL, which is what fits in an extent's owner
 * field and is therefore not negotiable.  validate() has already established
 * the former is no larger than the latter.
 *
 * Scale the layout down proportionally rather than refusing to boot, but say
 * so: a silently smaller fan-out than asked for is the kind of thing that gets
 * mistaken for a tuning result.
 */
JET_EXTERN void
hpa_pool_layout_clamp(hpa_pool_layout_t *layout) {
	/*
	 * 64-bit: validate() has already bounded each pool, but the sum of up
	 * to HPA_MAX_POOLS of them still must not wrap, or an over-large
	 * request could masquerade as a small one and skip clamping entirely.
	 */
	unsigned limit = layout->nshards_max;
	assert(limit >= layout->npools && limit <= HPA_MAX_SHARDS_TOTAL);

	uint64_t requested = 0;
	for (unsigned i = 0; i < layout->npools; i++) {
		requested += layout->pools[i].nshards;
	}
	if (requested <= limit) {
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
	unsigned remaining = limit;
	unsigned total = 0;
	for (unsigned i = 0; i < layout->npools; i++) {
		unsigned pools_after = layout->npools - i - 1;
		assert(remaining > pools_after);
		unsigned budget = remaining - pools_after;
		unsigned scaled = (unsigned)(((uint64_t)layout->pools[i].nshards
		    * limit) / requested);
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
	assert(total <= limit);

	malloc_printf("<jemalloc>: hpa pools: requested %" FMTu64 " shards, "
	    "clamped to %u (limit %u)\n", requested, total, limit);
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
		hpa_pool_opts_apply(&pool->opts, &pool->sec_opts,
		    &local.pools[i]);
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

/*
 * The process-wide pool set.  Zero-initialised, so hpa_pools_ready() is false
 * until hpa_pools_boot() says otherwise -- which matters during bootstrap,
 * when arena 0 exists before the pools do.
 */
hpa_pool_set_t hpa_pools_global;

/*
 * Fork.  One pass over every shard, at the same phase numbers the per-arena
 * code used, so the order relative to PAC and arena locks is unchanged.  The
 * arena walk cannot do this any more: shards are shared, so it would take each
 * mutex once per arena and deadlock against itself in the parent.
 */
void
hpa_pools_prefork2(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_prefork2(tsdn, &hpa_pools_global.shards[i]);
	}
}

void
hpa_pools_prefork3(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_prefork3(tsdn, &hpa_pools_global.shards[i]);
	}
}

void
hpa_pools_prefork4(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_prefork4(tsdn, &hpa_pools_global.shards[i]);
	}
}

/*
 * The shared edata cache is a lock as well as a lifetime fix, and it is inner
 * to the shard mutexes: hpa_try_alloc_one_offset() calls
 * edata_cache_fast_get() while holding one.  So it is taken last, at the same
 * phase the per-arena caches use.
 */
void
hpa_pools_prefork5(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	edata_cache_prefork(tsdn, hpa_pools_global.edata_cache);
}

void
hpa_pools_postfork_parent(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	edata_cache_postfork_parent(tsdn, hpa_pools_global.edata_cache);
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_postfork_parent(tsdn, &hpa_pools_global.shards[i]);
	}
}

void
hpa_pools_postfork_child(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	edata_cache_postfork_child(tsdn, hpa_pools_global.edata_cache);
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_postfork_child(tsdn, &hpa_pools_global.shards[i]);
	}
}

void
hpa_pools_set_deferral_allowed(tsdn_t *tsdn, bool deferral_allowed) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_set_deferral_allowed(
		    tsdn, &hpa_pools_global.shards[i], deferral_allowed);
	}
}

/*
 * Every background thread drives every shard, rather than taking a stripe.
 *
 * Striping is what the arena walk does, but it does not transfer: background
 * threads are created on demand, and only for indices that some *existing
 * arena* maps to.  A shard whose stripe belongs to a thread that was never
 * created would simply never be driven -- and the failure is silent, showing
 * up as RSS climbing over hours rather than as anything a test catches.  That
 * is exactly the bug this hit in hpa_background_thread.
 *
 * Driving everything from each thread is correct regardless of which threads
 * exist.  The redundancy is cheap: the work is bounded and taken under the
 * shard mutex, so a second thread finds nothing to do.
 */
void
hpa_pools_do_deferred_work(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_do_deferred_work(tsdn, &hpa_pools_global.shards[i]);
	}
}

uint64_t
hpa_pools_time_until_deferred_work(tsdn_t *tsdn) {
	uint64_t time = DEFERRED_WORK_MAX;
	if (!hpa_pools_ready()) {
		return time;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		uint64_t shard_time = hpa_time_until_deferred_work(
		    tsdn, &hpa_pools_global.shards[i]);
		if (shard_time < time) {
			time = shard_time;
		}
		if (time == DEFERRED_WORK_MIN) {
			break;
		}
	}
	return time;
}

void
hpa_pools_stats_merge(tsdn_t *tsdn, hpa_shard_stats_t *dst,
    hpa_pool_stats_t *pool_dst, unsigned npools) {
	if (pool_dst != NULL) {
		memset(pool_dst, 0, npools * sizeof(hpa_pool_stats_t));
	}
	if (!hpa_pools_ready()) {
		return;
	}
	/*
	 * One pass over each shard filling both views, rather than two.  Each
	 * hpa_shard_stats_merge() takes the shard's mutexes, so a second walk
	 * would double the lock traffic and -- worse -- sample at a different
	 * instant, leaving the per-pool figures able to disagree with the
	 * merged ones from the same epoch.
	 */
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		hpa_pool_t       *pool = &hpa_pools_global.pools[i];
		hpa_pool_stats_t *out = (pool_dst != NULL && i < npools)
		    ? &pool_dst[i]
		    : NULL;
		for (unsigned j = 0; j < pool->nshards; j++) {
			hpa_shard_t *shard
			    = &hpa_pools_global.shards[pool->first_shard + j];

			hpa_shard_stats_t stats;
			memset(&stats, 0, sizeof(stats));
			hpa_shard_stats_merge(tsdn, shard, &stats);
			hpa_shard_stats_accum(dst, &stats);
			if (out == NULL) {
				continue;
			}

			/*
			 * Contention, from the same shards in the same pass.
			 * Taken separately from the figures above because
			 * hpa_shard_stats_merge() releases the mutexes before
			 * returning, and reading a lock's profile while
			 * holding it would count this read.
			 */
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

			/*
			 * slabs[] is the huge/non-huge split; merged is the sum
			 * of the two.  Both halves are reported because the
			 * question a per-pool threshold is tuned against is
			 * precisely how much of the pool went huge.
			 */
			psset_stats_t *ps = &stats.psset_stats;
			out->npageslabs_nonhuge += ps->slabs[0].npageslabs;
			out->npageslabs_huge += ps->slabs[1].npageslabs;
			out->nactive_nonhuge += ps->slabs[0].nactive;
			out->nactive_huge += ps->slabs[1].nactive;
			out->ndirty_nonhuge += ps->slabs[0].ndirty;
			out->ndirty_huge += ps->slabs[1].ndirty;

			out->npurge_passes
			    += stats.nonderived_stats.npurge_passes;
			out->npurges += stats.nonderived_stats.npurges;
			out->nhugifies += stats.nonderived_stats.nhugifies;
			out->nhugify_failures
			    += stats.nonderived_stats.nhugify_failures;
			out->ndehugifies += stats.nonderived_stats.ndehugifies;
		}
	}
}

void
hpa_pools_mtx_prof_reset(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_t *shard = &hpa_pools_global.shards[i];

		malloc_mutex_lock(tsdn, &shard->grow_mtx);
		malloc_mutex_prof_data_reset(tsdn, &shard->grow_mtx);
		malloc_mutex_unlock(tsdn, &shard->grow_mtx);

		malloc_mutex_lock(tsdn, &shard->mtx);
		malloc_mutex_prof_data_reset(tsdn, &shard->mtx);
		malloc_mutex_unlock(tsdn, &shard->mtx);

		sec_mutex_prof_reset(tsdn, &shard->sec);
	}
}

void
hpa_pools_flush(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_flush(tsdn, &hpa_pools_global.shards[i]);
	}
}

/*
 * "Give back what you can, now."
 *
 * The arena-scoped equivalents (arena.<i>.purge and friends) deliberately no
 * longer reach the HPA: a shard belongs to no arena, so no arena can speak for
 * it.  That would leave hugepage memory unreachable from mallctl altogether,
 * which is a capability regression rather than a naming one -- releasing
 * memory under pressure is something callers actually rely on.  Hence a verb
 * of its own, scoped the way the resource is.
 *
 * Flush first, then force deferred work: flushing returns SEC-cached extents
 * to their pssets, which is what makes the subsequent purge able to see them.
 */
void
hpa_pools_purge(tsdn_t *tsdn) {
	if (!hpa_pools_ready()) {
		return;
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_flush(tsdn, &hpa_pools_global.shards[i]);
	}
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		hpa_shard_do_deferred_work(tsdn, &hpa_pools_global.shards[i]);
	}
}
