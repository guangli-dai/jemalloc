#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/background_thread.h"
#include "jemalloc/internal/background_thread_inlines.h"
#include "jemalloc/internal/deferral.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/pa.h"

static void
pa_nactive_add(pa_shard_t *shard, size_t add_pages) {
	atomic_fetch_add_zu(&shard->nactive, add_pages, ATOMIC_RELAXED);
}

static void
pa_nactive_sub(pa_shard_t *shard, size_t sub_pages) {
	assert(pa_shard_nactive(shard) >= sub_pages);
	atomic_fetch_sub_zu(&shard->nactive, sub_pages, ATOMIC_RELAXED);
}

bool
pa_central_init(pa_central_t *central, base_t *base, bool hpa,
    const hpa_hooks_t *hpa_hooks) {
	bool err;
	if (hpa) {
		err = hpa_central_init(&central->hpa, base, hpa_hooks);
		if (err) {
			return true;
		}
	}
	return false;
}

bool
pa_shard_init(tsdn_t *tsdn, pa_shard_t *shard, pa_central_t *central,
    emap_t *emap, base_t *base, unsigned ind, pa_shard_stats_t *stats,
    malloc_mutex_t *stats_mtx, nstime_t *cur_time,
    size_t pac_oversize_threshold, ssize_t dirty_decay_ms,
    ssize_t muzzy_decay_ms) {
	/* This will change eventually, but for now it should hold. */
	assert(base_ind_get(base) == ind);
	if (edata_cache_init(&shard->edata_cache, base)) {
		return true;
	}

	if (pac_init(tsdn, &shard->pac, base, emap, &shard->edata_cache,
	        cur_time, pac_oversize_threshold, dirty_decay_ms,
	        muzzy_decay_ms, &stats->pac_stats, stats_mtx)) {
		return true;
	}

	shard->ind = ind;

	atomic_store_b(&shard->use_hpa, false, ATOMIC_RELAXED);

	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);

	shard->stats_mtx = stats_mtx;
	shard->stats = stats;
	memset(shard->stats, 0, sizeof(*shard->stats));

	shard->central = central;
	shard->emap = emap;
	shard->base = base;

	return false;
}

void
pa_shard_set_use_hpa(pa_shard_t *shard, bool use_hpa) {
	atomic_store_b(&shard->use_hpa, use_hpa, ATOMIC_RELAXED);
}

void
pa_shard_disable_hpa(tsdn_t *tsdn, pa_shard_t *shard) {
	/*
	 * Flag only.  This used to flush and disable the arena's own HPA
	 * shard, which was fine when it had one to itself; the shards are
	 * shared now, so doing that here would discard extents cached on
	 * behalf of every other arena.  Stopping this arena from routing new
	 * allocations to the HPA is the whole of what the caller wants, and
	 * extents it already owns keep going home to their own shard.
	 */
	(void)tsdn;
	pa_shard_set_use_hpa(shard, false);
}

void
pa_shard_reset(tsdn_t *tsdn, pa_shard_t *shard) {
	atomic_store_zu(&shard->nactive, 0, ATOMIC_RELAXED);
	pa_shard_flush(tsdn, shard, /* all */ false);
}

void
pa_shard_flush(tsdn_t *tsdn, pa_shard_t *shard, bool all) {
	pac_sec_flush(tsdn, &shard->pac);
	if (all) {
		pac_decay_all_now(tsdn, &shard->pac, extent_state_dirty);
		if (pac_should_decay_muzzy(&shard->pac)) {
			pac_decay_all_now(tsdn, &shard->pac, extent_state_muzzy);
		}
	}
}

static bool
pa_shard_uses_hpa(pa_shard_t *shard) {
	return atomic_load_b(&shard->use_hpa, ATOMIC_RELAXED);
}

void
pa_shard_destroy(tsdn_t *tsdn, pa_shard_t *shard) {
	/*
	 * PAC only.  The HPA shards outlive any single arena -- an extent this
	 * arena allocated may still be cached in a shared SEC, and the shard
	 * serves other arenas regardless -- so there is nothing here to
	 * destroy on the HPA side.
	 */
	pac_destroy(tsdn, &shard->pac);
}

edata_t *
pa_alloc(tsdn_t *tsdn, pa_shard_t *shard, size_t size, size_t alignment,
    bool slab, szind_t szind, bool zero, bool guarded,
    bool *deferred_work_generated) {
	witness_assert_depth_to_rank(
	    tsdn_witness_tsdp_get(tsdn), WITNESS_RANK_CORE, 0);
	assert(!guarded || alignment <= PAGE);

	edata_t     *edata = NULL;
	hpa_shard_t *hpa = NULL;
	/*
	 * size <= HUGEPAGE is a precondition of routing, not a policy: a
	 * larger extent has no page-size class in the routing table and could
	 * never be served by a pageslab anyway.  hpa_alloc() used to absorb
	 * these and return NULL, but the decision now happens one step
	 * earlier, so the check has to move with it.  Whether a size that
	 * *can* be routed should be served is still hpa_alloc()'s call
	 * (slab_max_alloc, frequent_reuse).
	 */
	if (!guarded && size <= HUGEPAGE && pa_shard_uses_hpa(shard)) {
		/*
		 * Step 1 picks the pool from the requested size class; step 2
		 * lets that pool pick one of its own shards.  The arena index
		 * is only a hint -- with the identity layout and an arena below
		 * the boot-time shard count it selects shard == arena, which is
		 * the old topology.
		 */
		hpa = hpa_route(&hpa_pools_global, size, slab, szind,
		    /* hint */ shard->ind);
		edata = hpa_alloc(tsdn, hpa, size, alignment, zero,
		    /* guarded */ false, slab, deferred_work_generated);
	}
	/*
	 * Fall back to the PAC if the HPA is off or couldn't serve the given
	 * allocation request.
	 */
	if (edata == NULL) {
		edata = pac_alloc(tsdn, &shard->pac, size, alignment, zero,
		    guarded, slab, deferred_work_generated);
	}
	if (edata != NULL) {
		assert(edata_size_get(edata) == size);
		/*
		 * This is the single point at which an extent acquires an
		 * owning arena, and it has to happen before anything reads one.
		 *
		 * HPA extents arrive unowned: either freshly carved, or served
		 * from the SEC, where they may have been parked by a different
		 * arena entirely.  PAC extents come from this arena's own
		 * caches and already carry the right index.
		 */
		if (edata_pai_get(edata) == EXTENT_PAI_HPA) {
			/*
			 * Whether it was carved just now or served from the
			 * SEC, it must have arrived unowned -- an HPA extent
			 * carrying an arena at this point means someone failed
			 * to relinquish it on the way in, and we would be
			 * silently overwriting the evidence.
			 */
			assert(edata_arena_ind_get_maybe_unassociated(edata)
			    == EDATA_ARENA_IND_UNASSOCIATED);
			edata_arena_ind_set(edata, shard->ind);
			/*
			 * The extent came from the shard we routed to.  True
			 * for SEC hits as well as fresh carvings: an extent
			 * only ever enters the SEC of its own owner, because
			 * hpa_dalloc() reaches the SEC through
			 * hpa_shard_from_edata().
			 *
			 * Note this is emphatically *not* the arena index.
			 * Even under the identity layout the two diverge as
			 * soon as an arena exists beyond the boot-time shard
			 * count -- arenas.create() does that -- and later
			 * arenas then share shards with earlier ones.  Shards
			 * are a resource sized once at boot, not a per-arena
			 * possession.
			 */
			assert(hpa_shard_from_edata(edata) == hpa);
		}
		assert(edata_arena_ind_get(edata) == shard->ind);
		pa_nactive_add(shard, size >> LG_PAGE);
		emap_remap(tsdn, shard->emap, edata, szind, slab);
		edata_szind_set(edata, szind);
		edata_slab_set(edata, slab);
		if (slab && (size > 2 * PAGE)) {
			emap_register_interior(tsdn, shard->emap, edata, szind);
		}
	}
	return edata;
}

bool
pa_expand(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool zero, bool *deferred_work_generated) {
	assert(new_size > old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);
	if (edata_guarded_get(edata)) {
		return true;
	}
	size_t expand_amount = new_size - old_size;

	/*
	 * HPA expand always fails (it's a stub); skip the call entirely for
	 * HPA-owned extents.
	 */
	if (edata_pai_get(edata) == EXTENT_PAI_HPA) {
		return true;
	}
	bool error = pac_expand(tsdn, &shard->pac, edata, old_size, new_size,
	    zero, deferred_work_generated);
	if (error) {
		return true;
	}

	pa_nactive_add(shard, expand_amount >> LG_PAGE);
	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

bool
pa_shrink(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata, size_t old_size,
    size_t new_size, szind_t szind, bool *deferred_work_generated) {
	assert(new_size < old_size);
	assert(edata_size_get(edata) == old_size);
	assert((new_size & PAGE_MASK) == 0);
	if (edata_guarded_get(edata)) {
		return true;
	}
	size_t shrink_amount = old_size - new_size;

	/*
	 * HPA shrink always fails (it's a stub); skip the call entirely for
	 * HPA-owned extents.
	 */
	if (edata_pai_get(edata) == EXTENT_PAI_HPA) {
		return true;
	}
	bool error = pac_shrink(tsdn, &shard->pac, edata, old_size, new_size,
	    deferred_work_generated);
	if (error) {
		return true;
	}
	pa_nactive_sub(shard, shrink_amount >> LG_PAGE);

	edata_szind_set(edata, szind);
	emap_remap(tsdn, shard->emap, edata, szind, /* slab */ false);
	return false;
}

void
pa_dalloc(tsdn_t *tsdn, pa_shard_t *shard, edata_t *edata,
    bool *deferred_work_generated) {
	emap_remap(tsdn, shard->emap, edata, SC_NSIZES, /* slab */ false);
	if (edata_slab_get(edata)) {
		emap_deregister_interior(tsdn, shard->emap, edata);
		/*
		 * The slab state of the extent isn't cleared.  It may be used
		 * by the pai implementation, e.g. to make caching decisions.
		 */
	}
	edata_addr_set(edata, edata_base_get(edata));
	edata_szind_set(edata, SC_NSIZES);
	pa_nactive_sub(shard, edata_size_get(edata) >> LG_PAGE);
	if (edata_pai_get(edata) == EXTENT_PAI_HPA) {
		/*
		 * Relinquish the arena as the extent leaves it.  An HPA extent
		 * can be cached in the SEC and reissued to a different arena,
		 * so anything read from here on is stale by construction; make
		 * that a loud failure rather than a plausible wrong answer.
		 */
		edata_arena_ind_set(edata, EDATA_ARENA_IND_UNASSOCIATED);
		hpa_dalloc(tsdn, hpa_shard_from_edata(edata), edata,
		    deferred_work_generated);
	} else {
		pac_dalloc(tsdn, &shard->pac, edata, deferred_work_generated);
	}
}

bool
pa_decay_ms_set(tsdn_t *tsdn, pa_shard_t *shard, extent_state_t state,
    ssize_t decay_ms) {
	return pac_decay_ms_set(tsdn, &shard->pac, state, decay_ms);
}

ssize_t
pa_decay_ms_get(pa_shard_t *shard, extent_state_t state) {
	return pac_decay_ms_get(&shard->pac, state);
}


void
pa_shard_handle_deferred_work(tsdn_t *tsdn, pa_shard_t *shard) {
	witness_assert_depth_to_rank(
	    tsdn_witness_tsdp_get(tsdn), WITNESS_RANK_CORE, 0);

	if (pac_decay_immediately(&shard->pac)) {
		pac_decay_all_now(tsdn, &shard->pac, extent_state_dirty);
	}
	if (background_thread_enabled()) {
		pac_wake_bg_on_deferred(tsdn, &shard->pac);
	}
}

void
pa_shard_do_deferred_work(
    tsdn_t *tsdn, pa_shard_t *shard, bool is_background_thread) {
	/*
	 * PAC only.  HPA deferred work is no longer per arena: shards are
	 * shared, so driving them from the arena walk would drive each one
	 * once per arena.  The background thread drives them in its own pass
	 * over the pool set -- see hpa_pools_do_deferred_work().
	 */
	pac_do_deferred_work(tsdn, &shard->pac, is_background_thread);
}

/*
 * Get time until next deferred work ought to happen. If there are multiple
 * things that have been deferred, this function calculates the time until
 * the soonest of those things.
 */
uint64_t
pa_shard_time_until_deferred_work(tsdn_t *tsdn, pa_shard_t *shard) {
	/* PAC only; the HPA half is folded in by the caller's pool pass. */
	return pac_time_until_deferred_work(tsdn, &shard->pac);
}
