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
 * What layout to build.  Kept separate from the pool set itself so that the
 * configuration parser can fill one in without knowing anything about how
 * pools are constructed, and so that tests can build layouts directly.
 */
typedef struct hpa_pool_layout_s hpa_pool_layout_t;
struct hpa_pool_layout_s {
	unsigned npools;
	struct {
		/* Upper bound of this pool's band; the lower bound is implied
		 * by the previous pool.  The last pool must reach HUGEPAGE. */
		size_t   size_max;
		unsigned nshards;
	} pools[HPA_MAX_POOLS];
	hpa_pool_pick_t pick;
};

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
 * The identity layout: one pool spanning everything, one shard per arena,
 * picked by arena index.  This reproduces the pre-pool topology exactly, and
 * is both the default and the thing a rollback switch selects.
 */
void hpa_pool_layout_identity(hpa_pool_layout_t *layout, unsigned narenas);

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
