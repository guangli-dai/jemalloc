#ifndef JEMALLOC_INTERNAL_HPA_POOL_H
#define JEMALLOC_INTERNAL_HPA_POOL_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/edata.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/mutex_prof.h"
#include "jemalloc/internal/psset.h"
#include "jemalloc/internal/sz.h"

/*
 * Size-routed HPA shard pools.
 *
 * An HPA shard used to be a member of arena_t.pa_shard, which made "which
 * arena is this thread bound to" and "which hugepages does this extent come
 * from" the same question.  A pool set answers them separately: page-level
 * allocations are routed to a pool by the size of the extent being requested,
 * and the pool then picks one of its own shards however it likes.
 *
 * Allocation is two decisions:
 *
 *   1. size -> pool.  Forced.  A total function over [PAGE, HUGEPAGE], fixed
 *      at boot, so the same key always yields the same pool.  This is what
 *      buys segregation: extents of very different sizes cannot land in the
 *      same psset, and therefore cannot share a hugepage.
 *
 *   2. pool -> one of that pool's own shards.  Free.  Every shard in a pool
 *      serves the same size band under the same policy, so the pool may
 *      spread load however it wants, and may change how it does so at any
 *      time.  A bad picker can cost packing or contention; it cannot be
 *      wrong.
 *
 * Deallocation makes no decision at all.  An extent goes back to the shard
 * that served it, recorded in the extent itself (EDATA_BITS_HPA_SHARD).  Step
 * 1 would be invertible -- extent sizes are stable, since the HPA refuses
 * expand and shrink -- but step 2 is not: nothing about an extent says which
 * of its pool's shards produced it.  That asymmetry is the whole reason the
 * owning shard has to be recorded rather than recomputed.
 */

/*
 * How a pool chooses among its own shards.  Deliberately boring to start
 * with: build the mechanism, validate it, then explore the policy space with
 * something to measure against.
 *
 * Load-aware pickers (choose the emptiest psset, say) are the obvious next
 * idea and are NOT safely implementable as naively written: psset state is
 * guarded by each shard's mtx, and every shard mutex shares one exclusive
 * witness rank, so a picker that scans shards either reads mutable state
 * unsynchronized or holds two same-rank mutexes.  Any future load-aware
 * picker needs an atomic occupancy summary read outside the lock, or trylock
 * sampling that gives up rather than blocking.
 */
typedef enum hpa_pool_pick_e {
	/*
	 * shard = hint % nshards, where the hint is the calling arena's index.
	 * With one pool and nshards == narenas this reproduces the historical
	 * topology exactly: arena i always allocates from shard i.
	 */
	hpa_pool_pick_arena = 0,
	/* Atomic counter.  Spreads load without reference to arena identity. */
	hpa_pool_pick_roundrobin = 1,
	hpa_pool_pick_limit
} hpa_pool_pick_t;

extern const char *const hpa_pool_pick_names[];

/*
 * Routing table size: one entry per page-size class that can describe an
 * extent the HPA is willing to serve, i.e. up to HUGEPAGE.  This is the same
 * bound the psset uses to index its own per-pszind structures, for the same
 * reason.  hpa_pools_boot() asserts the bound actually covers HUGEPAGE.
 */
#define HPA_ROUTE_NKEYS PSSET_NPSIZES

/*
 * Two separate preconditions, both checked rather than assumed.  Routing needs
 * the table to cover every size the HPA can serve, which hpa_pools_boot()
 * asserts against sz_psz2ind(HUGEPAGE).  Building the table is stricter: it
 * calls sz_pind2sz() for *every* key, whose own precondition is the full
 * page-size-class range.  A configuration that satisfied the first and broke
 * the second would trip an assert deep inside sz.h at boot rather than say
 * anything useful here.
 */
#if HPA_ROUTE_NKEYS > SC_NPSIZES + 1
#error "HPA_ROUTE_NKEYS exceeds the valid sz_pind2sz() domain"
#endif

/* A cap on pools, purely so the set can be a fixed-size struct. */
#define HPA_MAX_POOLS 8

/*
 * Default ceiling on the total number of shards in the process.
 *
 * Shards used to be created with their arena, so a process with narenas=704
 * but five live arenas had five shards.  Pools are built once at boot and
 * cannot know which arenas will ever exist, so sizing the default layout at
 * narenas would allocate 704 of them -- roughly 7 MiB of psset and SEC
 * metadata for a process that will use a handful.
 *
 * Capping trades exact topology preservation for bounded memory: above this
 * many arenas, several share a shard.  That is a real deviation from the
 * pre-pool behaviour and is called out in the design docs; it is also, for
 * what it is worth, the direction this project is trying to go, since a shard
 * per arena is what hurts hugepage locality in the first place.
 *
 * 64 is a first-version number, not a measured one.  It is the default of
 * opt_hpa_pool_nshards_max rather than a compile-time constant so that raising
 * it is a MALLOC_CONF experiment instead of a rebuild; the hard ceiling is
 * HPA_MAX_SHARDS_TOTAL, set by the width of the extent's owner field.
 */
#define HPA_POOL_NSHARDS_MAX_DEFAULT 64

typedef struct hpa_pool_s hpa_pool_t;
struct hpa_pool_s {
	/*
	 * The extent-size band this pool serves, in bytes, inclusive at both
	 * ends.  Note these are *extent* sizes, not the sizes an application
	 * asked for: the bins channel passes bin_info->slab_size, and the
	 * large channel passes usize + sz_large_pad, which with
	 * cache-oblivious layout is a page more than the user requested.
	 */
	size_t size_min;
	size_t size_max;

	/* Per-pool policy.  Distinct bands want distinct purge/hugify rules. */
	hpa_shard_opts_t opts;
	sec_opts_t       sec_opts;

	/*
	 * This pool owns hpa_pool_set_t.shards[first_shard ..
	 * first_shard + nshards).  A contiguous slice, so picking a shard is
	 * an index add.
	 */
	unsigned first_shard;
	unsigned nshards;

	hpa_pool_pick_t pick;
	/* Cursor for hpa_pool_pick_roundrobin. */
	atomic_u_t rr_next;
};

/*
 * Per-pool option overrides.
 *
 * Distinct size bands want distinct policy -- that is most of the point of
 * having bands.  The load-bearing case is hugification: the global threshold is
 * 95% of a hugepage, so a pool of ~1 MiB extents never hugifies on one extent
 * and always does on two.  Without a threshold of its own, such a pool produces
 * permanently non-huge, half-used hugepages, which is the outcome the whole
 * exercise exists to avoid.
 *
 * Overrides are recorded as (value, "was it set") rather than as absolute
 * values, because MALLOC_CONF is order-independent: the global default a pool
 * inherits may be parsed after the override that refines it.  The set bits say
 * which fields to apply on top of whatever the globals ended up being.
 */
/*
 * The settable options, as one list.
 *
 * Four things have to agree for an override to work: the enumerator, the name
 * MALLOC_CONF uses, the parser that reads the value, and the fold that applies
 * it over the process-wide default.  Three of them are generated from the list
 * below, so they cannot drift.  The fourth, the parser, is hand-written per
 * option because each value has its own type and range -- but an option
 * missing from it falls through to a "no such option" error rather than being
 * silently accepted, which is the safe direction.
 *
 * The name is the struct field name, so that the fold can be generated: an
 * hpa_shard_opts_t field is named exactly as the option is, and a sec_opts_t
 * field is named as the option minus its "sec_" prefix.
 */
#define HPA_POOL_SHARD_OPTS                                                    \
	OP(slab_max_alloc)                                                     \
	OP(hugification_threshold)                                             \
	OP(dirty_mult)                                                         \
	OP(hugify_delay_ms)                                                    \
	OP(hugify_sync)                                                        \
	OP(min_purge_interval_ms)                                              \
	OP(purge_threshold)                                                    \
	OP(min_purge_delay_ms)                                                 \
	OP(hugify_style)

#define HPA_POOL_SEC_OPTS                                                      \
	OP(nshards)                                                            \
	OP(max_alloc)                                                          \
	OP(max_bytes)

typedef enum hpa_pool_opt_e {
#define OP(field) hpa_pool_opt_##field,
	HPA_POOL_SHARD_OPTS
#undef OP
#define OP(field) hpa_pool_opt_sec_##field,
	HPA_POOL_SEC_OPTS
#undef OP
	    hpa_pool_opt_limit
} hpa_pool_opt_t;

/* Names as they appear in MALLOC_CONF, indexed by hpa_pool_opt_t. */
extern const char *const hpa_pool_opt_names[];

typedef struct hpa_pool_layout_entry_s hpa_pool_layout_entry_t;
struct hpa_pool_layout_entry_s {
	/*
	 * Upper bound of this pool's band; the lower bound is implied by the
	 * previous pool.  The last pool must reach HUGEPAGE.
	 */
	size_t   size_max;
	unsigned nshards;

	/*
	 * Only the fields named in opts_set are meaningful; the rest are
	 * filled from the process-wide defaults when the pool is built.
	 */
	hpa_shard_opts_t opts;
	sec_opts_t       sec_opts;
	uint32_t         opts_set;
};

/*
 * What layout to build.  Kept separate from the pool set itself so that the
 * configuration parser can fill one in without knowing anything about how
 * pools are constructed, and so that tests can build layouts directly.
 */
typedef struct hpa_pool_layout_s hpa_pool_layout_t;
struct hpa_pool_layout_s {
	unsigned                npools;
	hpa_pool_layout_entry_t pools[HPA_MAX_POOLS];
	hpa_pool_pick_t         pick;

	/*
	 * Ceiling on the sum of the nshards above.  A layout asking for more is
	 * scaled down proportionally rather than rejected, since the figure a
	 * caller writes (narenas, say) is a wish rather than a requirement.
	 */
	unsigned nshards_max;
};

/*
 * Set by MALLOC_CONF, consumed once by malloc_init_hard().
 *
 * opt_hpa_shard_pools is the feature switch.  When false -- the default --
 * malloc_init_hard() ignores the layout below and builds the identity layout
 * instead, reproducing the pre-pool topology.  Rollback in production is then
 * a MALLOC_CONF edit rather than a binary revert.
 */
extern bool              opt_hpa_shard_pools;
extern hpa_pool_layout_t opt_hpa_pool_layout;
extern size_t            opt_hpa_pool_nshards_max;

typedef struct hpa_pool_set_s hpa_pool_set_t;
struct hpa_pool_set_s {
	bool     initialized;
	unsigned npools;
	unsigned nshards_total;

	hpa_pool_t pools[HPA_MAX_POOLS];

	/*
	 * Every shard in the process, in one contiguous array allocated once
	 * at boot and never resized.  The id recorded on an extent is an index
	 * into this array, so deallocation is a single indexed load and needs
	 * no idea which pool the extent came from.
	 *
	 * Building it once, before any arena may route to the HPA, is also
	 * what makes publication ordering trivial: there is no window in which
	 * a shard is half-initialised and reachable.
	 */
	hpa_shard_t *shards;

	/*
	 * The cache every shard draws edata_t from.  Kept here because it is a
	 * lock as well as a lifetime fix: it must be forked exactly once, after
	 * all the shard mutexes, and whoever does that should not have to be
	 * told separately which cache the pools were built on.
	 */
	edata_cache_t *edata_cache;

	/* Step 1's table.  Indexed by hpa_route_key(). */
	hpa_pool_t *pool_by_key[HPA_ROUTE_NKEYS];
};

/*
 * The one pool set.  Built once by hpa_pools_boot() before any arena is
 * allowed to route to the HPA, and never resized, which is what makes
 * publication ordering a non-question: there is no window in which a shard is
 * half-initialised and reachable.
 */
extern hpa_pool_set_t hpa_pools_global;

static inline bool
hpa_pools_ready(void) {
	return hpa_pools_global.initialized;
}

/*
 * Does a given (arena, size) pair always reach the same shard?
 *
 * Only the arena picker promises that.  Round robin deliberately does not --
 * spreading is the point -- which invalidates any measurement of "the shard
 * this arena uses".  Tests that read one shard's psset directly are the
 * callers: without this they would be sampling a shard chosen at random and
 * reporting the result as a packing or purging failure.
 */
static inline bool
hpa_pools_shard_is_stable(void) {
	if (!hpa_pools_global.initialized) {
		return false;
	}
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		if (hpa_pools_global.pools[i].pick != hpa_pool_pick_arena) {
			return false;
		}
	}
	return true;
}

/*
 * The deallocation side of routing, and the reason the owning shard is
 * recorded on the extent at all.  One indexed load; no pool lookup, because
 * getting an extent home does not depend on which pool it came from.
 */
JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_shard_from_edata(const edata_t *edata) {
	assert(edata_pai_get(edata) == EXTENT_PAI_HPA);
	assert(hpa_pools_global.initialized);
	unsigned id = edata_hpa_shard_get(edata);
	assert(id < hpa_pools_global.nshards_total);
	return &hpa_pools_global.shards[id];
}

/*
 * Fork.  Shards are shared, so each mutex must be taken exactly once rather
 * than once per arena -- the arena walk that used to do this would now
 * self-deadlock.  Phase numbers match the arena ones so the order relative to
 * PAC and arena locks is unchanged: 2 = SEC, 3 = grow, 4 = shard, and 5 for
 * the shared edata cache, which is strictly inner to the shard mutex
 * (WITNESS_RANK_EDATA_CACHE vs WITNESS_RANK_HPA_SHARD) because
 * edata_cache_fast_get() runs while a shard mutex is held.
 */
void hpa_pools_prefork2(tsdn_t *tsdn);
void hpa_pools_prefork3(tsdn_t *tsdn);
void hpa_pools_prefork4(tsdn_t *tsdn);
void hpa_pools_prefork5(tsdn_t *tsdn);
void hpa_pools_postfork_parent(tsdn_t *tsdn);
void hpa_pools_postfork_child(tsdn_t *tsdn);

/* Deferred work, driven once per shard rather than once per arena. */
void hpa_pools_set_deferral_allowed(tsdn_t *tsdn, bool deferral_allowed);
void     hpa_pools_do_deferred_work(tsdn_t *tsdn);
uint64_t hpa_pools_time_until_deferred_work(tsdn_t *tsdn);

/*
 * Stats.  These are process-wide now: an HPA shard belongs to a pool, not an
 * arena, so there is no per-arena HPA figure to report.
 *
 * ctl takes the HPA share of stats.resident out of the merged psset stats
 * these produce, rather than sampling the pssets again: dropping the HPA
 * contribution entirely would silently under-report memory, and re-reading it
 * unlocked would let resident disagree with the HPA stats beside it.
 */
void hpa_pools_flush(tsdn_t *tsdn);

/*
 * Per-pool figures, which the merged view above cannot answer.
 *
 * Per-pool options are only tunable if they are also measurable: the point of
 * giving a 1 MiB band its own hugification threshold is that its pageslabs
 * reach full occupancy and hugify, and the merged number cannot say whether
 * they did.  Occupancy is nactive / (npageslabs * HUGEPAGE_PAGES), which is why
 * both halves are here alongside the hugify counters.
 *
 * A compact subset rather than the whole psset breakdown: this is what the
 * tuning sweep reads, and duplicating forty-odd nodes per pool would cost more
 * to maintain than it answers.
 */
typedef struct hpa_pool_stats_s hpa_pool_stats_t;
struct hpa_pool_stats_s {
	size_t   npageslabs_huge;
	size_t   npageslabs_nonhuge;
	size_t   nactive_huge;
	size_t   nactive_nonhuge;
	size_t   ndirty_huge;
	size_t   ndirty_nonhuge;
	uint64_t npurge_passes;
	uint64_t npurges;
	uint64_t nhugifies;
	uint64_t nhugify_failures;
	uint64_t ndehugifies;

	/*
	 * Contention, per pool.  The whole tradeoff the bands exist to make is
	 * packing against contention -- concentrating traffic into fewer,
	 * larger pssets is what lets extents pair, and is also what makes them
	 * contend -- so the sweep that picks band boundaries has to be able to
	 * see both halves for the same band.  The process-wide
	 * stats.mutexes.hpa_* sums cannot separate a busy band from a quiet
	 * one.
	 *
	 * Indexed by hpa_pool_mutex_t.
	 */
	mutex_prof_data_t mutexes[3];
};

typedef enum hpa_pool_mutex_e {
	hpa_pool_mutex_shard = 0,
	hpa_pool_mutex_shard_grow = 1,
	hpa_pool_mutex_sec = 2,
	hpa_pool_mutex_limit = 3
} hpa_pool_mutex_t;

/*
 * Fills both views in one walk: the merged figures in dst, and the per-pool
 * ones in pool_dst[0 .. min(npools, live pools)).  pool_dst may be NULL.
 * Together, because each shard's stats are read under its mutex and two walks
 * would sample at two instants.
 */
void hpa_pools_stats_merge(tsdn_t *tsdn, hpa_shard_stats_t *dst,
    hpa_pool_stats_t *pool_dst, unsigned npools);

/*
 * Mutex profiles are reset here rather than by whoever resets the arena ones,
 * for the same reason the figures are reported globally: no arena owns these
 * locks.  The figures themselves come out of the per-pool stats above, summed
 * by ctl -- walking the shards a second time to read them would count the
 * reader's own lock operations, and the whole would exceed the sum of its
 * parts.
 */
void hpa_pools_mtx_prof_reset(tsdn_t *tsdn);

/*
 * Flush every shard's SEC and then force its deferred work.  Backs the
 * hpa.purge mallctl -- the HPA-scoped replacement for what arena.<i>.purge
 * used to reach.
 */
void hpa_pools_purge(tsdn_t *tsdn);

/*
 * Build a layout incrementally, one band at a time -- what the configuration
 * parser drives.  hpa_pool_layout_add() takes the band inclusive at both ends
 * and checks it abuts the previous one, so a gap or an overlap is rejected
 * where it is written rather than surfacing later as a routing hole.
 */
void hpa_pool_layout_init(hpa_pool_layout_t *layout);
bool hpa_pool_layout_add(hpa_pool_layout_t *layout, size_t size_start,
    size_t size_end, unsigned nshards);

/*
 * Record one per-pool override, addressed by the band's size range rather than
 * by its position in the list.  A range that does not name a configured band
 * exactly is an error rather than a no-op: a silently ignored override reads as
 * "I tuned that and it did not help".
 *
 * Returns true on error, having complained.  The key and value are the text
 * from MALLOC_CONF; parsing lives here so that the range-to-pool lookup, the
 * name table and the field it fills stay in one place.
 */
bool hpa_pool_layout_set_opt(hpa_pool_layout_t *layout, size_t size_start,
    size_t size_end, const char *key, size_t keylen, const char *val,
    size_t vallen);

/*
 * Check the whole-layout properties that hpa_pool_layout_add() cannot see one
 * band at a time: reaching HUGEPAGE, page alignment, a shard for every pool,
 * a cap that leaves room for one each.  Returns true on error, having said
 * what is wrong.
 *
 * Called at boot, and also by the configuration reader on a layout the feature
 * switch is about to discard -- so that a layout which cannot work is rejected
 * where it is written rather than at the moment someone turns the switch on.
 */
bool hpa_pool_layout_validate(const hpa_pool_layout_t *layout);

/*
 * The identity layout: one pool spanning everything, one shard per arena,
 * picked by arena index.  This reproduces the pre-pool topology exactly, and
 * is both the default and the thing the rollback switch selects.
 *
 * Exactly, that is, up to nshards_max: past that many arenas some of them
 * share a shard.  See HPA_POOL_NSHARDS_MAX_DEFAULT.
 */
void hpa_pool_layout_identity(
    hpa_pool_layout_t *layout, unsigned narenas, unsigned nshards_max);

/*
 * Build the pool set.  Returns true on error, having emitted a message.  The
 * caller supplies the metadata base and the edata cache that every shard will
 * draw from; both must outlive every arena, since a pooled extent can be
 * created by one arena and freed by another.
 */
bool hpa_pools_boot(tsdn_t *tsdn, hpa_pool_set_t *set, base_t *base,
    hpa_central_t *central, emap_t *emap, edata_cache_t *edata_cache,
    const hpa_pool_layout_t *layout, const hpa_shard_opts_t *opts,
    const sec_opts_t *sec_opts);

/*
 * Step 1.  The key is derived from the extent size; slab and szind are
 * accepted and currently ignored.
 *
 * They are in the signature from the outset because they are free -- both are
 * already arguments to pa_alloc() -- and because keying on (slab, size)
 * instead of size alone is the most likely next refinement: the bins and
 * large channels do not even share a size domain, so one set of boundaries
 * has to describe two differently-shaped distributions.
 *
 * Having them here means that change costs no churn at the call sites.  It is
 * not, however, confined to this function: routing the same extent size to
 * different pools depending on channel needs a second table dimension, and a
 * layout that can express overlapping bands rather than one ascending list of
 * bounds.  What is bought now is the interface, not the implementation.
 */
JEMALLOC_ALWAYS_INLINE unsigned
hpa_route_key(size_t size, bool slab, szind_t szind) {
	(void)slab;
	(void)szind;
	assert(size <= HUGEPAGE);
	return (unsigned)sz_psz2ind(size);
}

JEMALLOC_ALWAYS_INLINE hpa_pool_t *
hpa_pool_lookup(hpa_pool_set_t *set, unsigned key) {
	assert(set->initialized);
	assert(key < HPA_ROUTE_NKEYS);
	return set->pool_by_key[key];
}

/* Step 2.  Ranges over this pool's own shards and no others. */
hpa_shard_t *hpa_pool_pick_shard(
    hpa_pool_set_t *set, hpa_pool_t *pool, unsigned hint);

/* The two composed: the whole of the allocation-side routing decision. */
JEMALLOC_ALWAYS_INLINE hpa_shard_t *
hpa_route(hpa_pool_set_t *set, size_t size, bool slab, szind_t szind,
    unsigned hint) {
	hpa_pool_t *pool = hpa_pool_lookup(set, hpa_route_key(size, slab,
	    szind));
	assert(pool != NULL);
	return hpa_pool_pick_shard(set, pool, hint);
}

#endif /* JEMALLOC_INTERNAL_HPA_POOL_H */
