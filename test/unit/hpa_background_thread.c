#include "test/jemalloc_test.h"
#include "test/sleep.h"

#include "jemalloc/internal/hpa_pool.h"

TEST_BEGIN(test_hpa_background_thread_a0_initialized) {
	/*
	 * Arena 0 has dedicated initialization path.  We'd like to make sure
	 * deferral_allowed value initialized correctly from the start of the
	 * application.
	 */
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!have_background_thread);
	test_skip_if(san_guard_enabled());
	/* No pools without the HPA; the rest of this has nothing to check. */
	test_skip_if(!opt_hpa);

	bool   enabled = false;
	size_t sz = sizeof(enabled);
	int err = mallctl("background_thread", (void *)&enabled, &sz, NULL, 0);
	expect_d_eq(err, 0, "Unexpected mallctl() failure");
	expect_true(enabled, "Background thread should be enabled");

	/*
	 * Shards are global now, built once at boot, so there is no longer an
	 * arena-0 special case to get wrong -- but the property this test was
	 * protecting still matters: deferral_allowed has to be right from the
	 * first allocation, not set later.
	 */
	expect_true(hpa_pools_ready(), "HPA pools should be built by now");
	expect_u_gt(hpa_pools_global.nshards_total, 0, "");
	for (unsigned i = 0; i < hpa_pools_global.nshards_total; i++) {
		expect_true(hpa_pools_global.shards[i].opts.deferral_allowed,
		    "shard %u should have deferral_allowed set at startup", i);
	}
}
TEST_END

static void
sleep_for_background_thread_interval(void) {
	/*
	 * The sleep interval set in our .sh file is 50ms.  So it likely will
	 * run if we sleep for four times that.
	 */
	sleep_ns(200 * 1000 * 1000);
}

static unsigned
create_arena(void) {
	unsigned arena_ind;
	size_t   sz;

	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", (void *)&arena_ind, &sz, NULL, 2),
	    0, "Unexpected mallctl() failure");
	return arena_ind;
}

/*
 * Empty-pageslab dirty pages on the shard this arena routes to.
 *
 * This used to be stats.arenas.<i>.hpa_shard, which was exactly this arena's
 * work because each arena owned a shard.  Shards are shared now, so the CTL
 * figure is process-wide and includes whatever arena 0 has been doing --
 * which is fatal for a test that asserts the count starts and ends at zero.
 * Read the shard directly instead; the arena is freshly created and is the
 * only thing driving it.
 */
/*
 * Only meaningful under the arena picker: hpa_route() with a spreading picker
 * returns a different shard each call, so this would sample at random.  The
 * test cases skip in that case; see hpa_pools_shard_is_stable().
 */
static hpa_shard_t *
test_shard(unsigned arena_ind) {
	assert(hpa_pools_shard_is_stable());
	return hpa_route(&hpa_pools_global, PAGE, /* slab */ false,
	    sz_size2index(PAGE),
	    /* hint */ arena_ind);
}

static size_t
get_empty_ndirty(unsigned arena_ind) {
	uint64_t epoch = 1;
	size_t   sz = sizeof(epoch);
	int      err = je_mallctl(
            "epoch", (void *)&epoch, &sz, (void *)&epoch, sizeof(epoch));
	expect_d_eq(0, err, "Unexpected mallctl() failure");

	psset_stats_t *stats = &test_shard(arena_ind)->psset.stats;
	return stats->empty_slabs[0].ndirty + stats->empty_slabs[1].ndirty;
}

static void
set_background_thread_enabled(bool enabled) {
	int err;
	err = je_mallctl(
	    "background_thread", NULL, NULL, &enabled, sizeof(enabled));
	expect_d_eq(0, err, "Unexpected mallctl failure");
}

static void
wait_until_thread_is_enabled(unsigned arena_id) {
	tsd_t *tsd = tsd_fetch();

	bool sleeping = false;
	int  iterations = 0;
	do {
		background_thread_info_t *info = background_thread_info_get(
		    arena_id);
		malloc_mutex_lock(tsd_tsdn(tsd), &info->mtx);
		malloc_mutex_unlock(tsd_tsdn(tsd), &info->mtx);
		sleeping = background_thread_indefinite_sleep(info);
		assert_d_lt(iterations, UINT64_C(1000000),
		    "Waiting for a thread to start for too long");
	} while (!sleeping);
}

static void
expect_purging(unsigned arena_ind) {
	size_t empty_ndirty = get_empty_ndirty(arena_ind);
	expect_zu_eq(0, empty_ndirty, "Expected arena to start unused.");

	void *ptrs[2];
	ptrs[0] = mallocx(PAGE, MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena_ind));
	ptrs[1] = mallocx(PAGE, MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena_ind));

	empty_ndirty = get_empty_ndirty(arena_ind);
	expect_zu_eq(0, empty_ndirty, "All pages should be active");

	dallocx(ptrs[0], MALLOCX_TCACHE_NONE);
	expect_true(empty_ndirty == 0 || empty_ndirty == 1,
	    "Unexpected extra dirty page count: %zu", empty_ndirty);

	/*
	 * Wait for at least hpa_min_purge_interval_ms to trigger purge on next
	 * deallocation.
	 */
	sleep_for_background_thread_interval();

	dallocx(ptrs[1], MALLOCX_TCACHE_NONE);
	empty_ndirty = get_empty_ndirty(arena_ind);
	expect_zu_eq(0, empty_ndirty, "There are should be no dirty pages");
}

static void
expect_deferred_purging(unsigned arena_ind) {
	size_t empty_ndirty;

	empty_ndirty = get_empty_ndirty(arena_ind);
	expect_zu_eq(0, empty_ndirty, "Expected arena to start unused.");

	/*
	 * It's possible that we get unlucky with our stats collection timing,
	 * and the background thread runs in between the deallocation and the
	 * stats collection.  So we retry 10 times, and see if we *ever* see
	 * deferred reclamation.
	 */
	bool observed_dirty_page = false;
	for (int i = 0; i < 10; i++) {
		void *ptr = mallocx(
		    PAGE, MALLOCX_TCACHE_NONE | MALLOCX_ARENA(arena_ind));
		empty_ndirty = get_empty_ndirty(arena_ind);
		expect_zu_eq(0, empty_ndirty, "All pages should be active");
		dallocx(ptr, MALLOCX_TCACHE_NONE);
		empty_ndirty = get_empty_ndirty(arena_ind);
		expect_true(empty_ndirty == 0 || empty_ndirty == 1 || opt_prof,
		    "Unexpected extra dirty page count: %zu", empty_ndirty);
		if (empty_ndirty > 0) {
			observed_dirty_page = true;
			break;
		}
	}
	expect_true(observed_dirty_page, "");

	/*
	 * Under high concurrency / heavy test load (e.g. using run_test.sh),
	 * the background thread may not get scheduled for a longer period of
	 * time.  Retry 100 times max before bailing out.
	 */
	unsigned retry = 0;
	while ((empty_ndirty = get_empty_ndirty(arena_ind)) > 0
	    && (retry++ < 100)) {
		sleep_for_background_thread_interval();
	}

	expect_zu_eq(0, empty_ndirty, "Should have seen a background purge");
}

TEST_BEGIN(test_hpa_background_thread_purges) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!have_background_thread);
	/* Skip since guarded pages cannot be allocated from hpa. */
	test_skip_if(san_guard_enabled());
	/* Measuring one shard needs this arena's work to stay on it. */
	test_skip_if(!hpa_pools_shard_is_stable());

	unsigned arena_ind = create_arena();
	/*
	 * Our .sh sets dirty mult to 0, so all dirty pages should get purged
	 * any time any thread frees.
	 */
	expect_deferred_purging(arena_ind);
}
TEST_END

TEST_BEGIN(test_hpa_background_thread_enable_disable) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());
	test_skip_if(!have_background_thread);
	/* Skip since guarded pages cannot be allocated from hpa. */
	test_skip_if(san_guard_enabled());
	/* Measuring one shard needs this arena's work to stay on it. */
	test_skip_if(!hpa_pools_shard_is_stable());

	unsigned arena_ind = create_arena();

	set_background_thread_enabled(false);
	expect_purging(arena_ind);

	set_background_thread_enabled(true);
	wait_until_thread_is_enabled(arena_ind);
	expect_deferred_purging(arena_ind);
}
TEST_END

int
main(void) {
	/*
	 * OK, this is a sort of nasty hack.  We don't want to add *another*
	 * config option for HPA (the intent is that it becomes available on
	 * more platforms over time, and we're trying to prune back config
	 * options generally.  But we'll get initialization errors on other
	 * platforms if we set hpa:true in the MALLOC_CONF (even if we set
	 * abort_conf:false as well).  So we reach into the internals and set
	 * them directly, but only if we know that we're actually going to do
	 * something nontrivial in the tests.
	 */
	if (config_stats && hpa_supported() && have_background_thread) {
		opt_hpa = true;
		opt_background_thread = true;
	}
	return test_no_reentrancy(
	    /*
	     * Unfortunately, order of tests is important here.  We need to
	     * make sure arena #0 initialized correctly, before we start
	     * turning background thread on and off in other tests.
	     */
	    test_hpa_background_thread_a0_initialized,
	    test_hpa_background_thread_purges,
	    test_hpa_background_thread_enable_disable);
}
