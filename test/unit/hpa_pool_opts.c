#include "test/jemalloc_test.h"

#include "test/hpa_pool.h"

/*
 * Per-pool policy, and the reason it is required rather than a refinement.
 *
 * The global hugification threshold is 95% of a hugepage.  A hugepage holding
 * one 1 MiB extent sits at 50% active and is never a hugification candidate;
 * only a pair crosses the bar.  So a band of ~1 MiB extents has exactly two
 * ways to behave, and which one it gets is a policy question the band has to
 * answer for itself:
 *
 *   - default threshold: pairs hugify, singletons never do.  Fine if the band
 *     stays busy enough to pair.
 *   - lowered threshold: singletons hugify too, at the cost of a hugepage that
 *     is half waste until its partner arrives.
 *
 * Without a per-pool knob only the first is reachable, and a band that does
 * not pair produces permanently non-huge, half-used hugepages -- which is the
 * outcome the whole exercise exists to avoid.
 *
 * The layout below gives the big band its own threshold and no hugify delay,
 * and leaves the small band on the global defaults, so the two can be compared
 * inside one process.
 */

/*
 * Band bounds and thresholds are byte literals -- MALLOC_CONF is a string --
 * so this configuration describes one page geometry and the test skips
 * elsewhere.
 */
#if LG_PAGE == 12 && LG_HUGEPAGE == 21
#define POOLS_CONF                                                             \
	"hpa_pools:4096-524288:1|524289-2097152:1,"                            \
	"hpa_pool_opts:524289-2097152:hugification_threshold=1048576"          \
	"|524289-2097152:hugify_delay_ms=0"                                    \
	"|524289-2097152:dirty_mult=-1"                                        \
	"|524289-2097152:sec_nshards=0"                                        \
	"|4096-524288:purge_threshold=8192,"
#define BIG_SIZE      ((size_t)1024 * 1024)
#define BIG_THRESHOLD ((size_t)1024 * 1024)
#else
#define POOLS_CONF ""
#define BIG_SIZE      ((size_t)1024 * 1024)
#define BIG_THRESHOLD ((size_t)1024 * 1024)
#endif

const char *malloc_conf = POOLS_CONF
    "hpa:true,"
    "hpa_shard_pools:true,"
    "hpa_slab_max_alloc:2097152,"
    "hpa_sec_nshards:0,"
    "cache_oblivious:false";

#define NPOOLS_EXPECTED 2
#define BIG_POOL        1
#define SMALL_POOL      0

#define SMALL_BAND_MAX ((size_t)512 * 1024)

/*
 * An outer MALLOC_CONF overrides a test's own settings, so the bands this file
 * asked for may not be the bands that got built.  Check the topology rather
 * than assume it: everything below is written against these two specific
 * bands, and asserting overrides on somebody else's layout would report the
 * override as a bug.
 */
static bool
pools_configured(void) {
	if (!hpa_supported() || !opt_hpa || opt_cache_oblivious
	    || !hpa_pools_ready()
	    || hpa_pools_global.npools != NPOOLS_EXPECTED) {
		return false;
	}
	return hpa_pools_global.pools[SMALL_POOL].size_max == SMALL_BAND_MAX
	    && hpa_pools_global.pools[BIG_POOL].size_max == HUGEPAGE;
}

static hpa_shard_t *
pool_shard(unsigned pool, unsigned j) {
	hpa_pool_t *p = &hpa_pools_global.pools[pool];
	assert_u_lt(j, p->nshards, "no such shard");
	return &hpa_pools_global.shards[p->first_shard + j];
}

/* Force the deferred work that actually performs hugification. */
static void
run_deferred_work(void) {
	expect_d_eq(mallctl("hpa.purge", NULL, NULL, NULL, 0), 0,
	    "hpa.purge failed");
}

TEST_BEGIN(test_overrides_reach_the_shards) {
	test_skip_if(!pools_configured());

	/*
	 * Read the shards, not the layout: the layout is what was asked for,
	 * and the thing that matters is what the pool was built with.
	 */
	hpa_shard_t *big = pool_shard(BIG_POOL, 0);
	hpa_shard_t *small = pool_shard(SMALL_POOL, 0);

	expect_zu_eq(big->opts.hugification_threshold, BIG_THRESHOLD,
	    "the big band did not get its own hugification threshold");
	expect_u64_eq(big->opts.hugify_delay_ms, 0,
	    "the big band did not get its own hugify delay");

	/*
	 * The small band overrode something else entirely, and must have kept
	 * every field it did not name.  This is the property that makes the
	 * overrides safe to use: they are a diff against the globals, not a
	 * replacement for them.
	 */
	expect_zu_eq(small->opts.hugification_threshold,
	    opt_hpa_opts.hugification_threshold,
	    "the small band lost the global hugification threshold");
	expect_u64_eq(small->opts.hugify_delay_ms, opt_hpa_opts.hugify_delay_ms,
	    "the small band lost the global hugify delay");
	expect_zu_eq(small->opts.purge_threshold, 8192,
	    "the small band did not get its own purge threshold");
	expect_u32_eq((uint32_t)big->opts.dirty_mult, (uint32_t)-1,
	    "the big band did not get its own dirty_mult");

	/* And the two bands must genuinely differ, or this proves nothing. */
	expect_true(
	    big->opts.hugification_threshold
	        != small->opts.hugification_threshold,
	    "both bands ended up with the same threshold");

	/* SEC sizing is per pool too, and reaches a different struct. */
	expect_zu_eq(big->sec.opts.nshards, 0,
	    "the big band did not get its own SEC sizing");
}
TEST_END

TEST_BEGIN(test_pairs_fill_one_pageslab) {
	test_skip_if(!config_stats);
	test_skip_if(!pools_configured());
	test_skip_if(san_guard_enabled());

	/*
	 * Two 1 MiB extents are exactly one hugepage.  Segregating them into a
	 * band of their own is what makes them land together; the assertion is
	 * that the pair occupies one pageslab at full occupancy rather than
	 * two at half.
	 */
	hpa_shard_t *shard = pool_shard(BIG_POOL, 0);
	/*
	 * Absolute counts, not deltas against a baseline: an earlier case in
	 * this process leaves a retained empty pageslab behind, and the pair
	 * then reuses it, so a delta of zero would look like a failure.  The
	 * band is this test's alone, so nothing else should be active in it.
	 */
	expect_zu_eq(shard->psset.stats.merged.nactive, 0,
	    "something else is using the big band; the counts below cannot "
	    "mean anything");

	void *a = mallocx(BIG_SIZE, MALLOCX_TCACHE_NONE);
	void *b = mallocx(BIG_SIZE, MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(a, "mallocx failed");
	expect_ptr_not_null(b, "mallocx failed");

	expect_zu_eq(shard->psset.stats.merged.npageslabs, 1,
	    "a pair of %zu-byte extents should share one pageslab", BIG_SIZE);
	expect_zu_eq(shard->psset.stats.merged.nactive, 2 * (BIG_SIZE / PAGE),
	    "the pair should account for its own pages and no others");
	expect_zu_eq(shard->psset.stats.merged.nactive, HUGEPAGE / PAGE,
	    "the pair should fill the pageslab exactly");
	expect_zu_eq(shard->psset.stats.full_slabs[0].npageslabs
	        + shard->psset.stats.full_slabs[1].npageslabs,
	    1, "the pageslab the pair fills should be counted as full");

	dallocx(b, MALLOCX_TCACHE_NONE);
	dallocx(a, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_lowered_threshold_hugifies) {
	test_skip_if(!config_stats);
	test_skip_if(!pools_configured());
	test_skip_if(san_guard_enabled());

	hpa_shard_t *shard = pool_shard(BIG_POOL, 0);
	expect_zu_eq(shard->psset.stats.merged.nactive, 0,
	    "something else is using the big band");

	/*
	 * One extent, half a hugepage.  Under the global threshold of 95% this
	 * would never be a hugification candidate; under the band's own
	 * threshold of exactly BIG_SIZE it is.
	 *
	 * The threshold alone is not enough, which is the more interesting
	 * half of why these options have to be per pool.  Hugifying a
	 * half-used hugepage makes its untouched pages resident, and
	 * hpa_hugify_blocked_by_ndirty() refuses when that would push the
	 * shard past dirty_mult * nactive -- at the default 25% a singleton is
	 * blocked no matter what the threshold says.  So the band sets
	 * dirty_mult:-1 too.  A band that wants singletons hugified needs both
	 * knobs, and both are per pool for that reason.
	 */
	void *p = mallocx(BIG_SIZE, MALLOCX_TCACHE_NONE);
	expect_ptr_not_null(p, "mallocx failed");
	run_deferred_work();

	if (shard->stats.nhugify_failures > 0) {
		/* The kernel declined; the policy still did its job. */
		dallocx(p, MALLOCX_TCACHE_NONE);
		test_skip("hugification refused by the kernel");
	}

	/*
	 * Assert the state, not the transition.  An earlier case in this
	 * process may already have hugified the pageslab this one reuses, in
	 * which case no new hugification happens and the counter does not move
	 * -- but the extent is on a hugepage either way, which is the property
	 * the threshold is there to produce.
	 */
	expect_zu_eq(shard->psset.stats.slabs[1].nactive, BIG_SIZE / PAGE,
	    "a %zu-byte extent should be on a hugified pageslab under a "
	    "%zu-byte threshold", BIG_SIZE, BIG_THRESHOLD);
	expect_zu_eq(shard->psset.stats.slabs[0].nactive, 0,
	    "no active pages should be left on a non-huge pageslab");
	expect_u64_ge(shard->stats.nhugifies, 1,
	    "the band never hugified anything");

	dallocx(p, MALLOCX_TCACHE_NONE);
}
TEST_END

TEST_BEGIN(test_singletons_do_not_thrash) {
	test_skip_if(!config_stats);
	test_skip_if(!pools_configured());
	test_skip_if(san_guard_enabled());

	/*
	 * Allocating and freeing the same extent repeatedly must not make the
	 * band hugify and dehugify in a loop.  A pageslab that is hugified on
	 * every allocation and dehugified on every free costs two madvise
	 * calls per allocation and gains nothing; that is the failure mode a
	 * lowered threshold could plausibly introduce.
	 */
	hpa_shard_t *shard = pool_shard(BIG_POOL, 0);
	uint64_t     base_hugifies = shard->stats.nhugifies;
	uint64_t     base_dehugifies = shard->stats.ndehugifies;

	for (unsigned i = 0; i < 16; i++) {
		void *p = mallocx(BIG_SIZE, MALLOCX_TCACHE_NONE);
		expect_ptr_not_null(p, "mallocx failed");
		run_deferred_work();
		dallocx(p, MALLOCX_TCACHE_NONE);
		run_deferred_work();
	}

	uint64_t hugifies = shard->stats.nhugifies - base_hugifies;
	uint64_t dehugifies = shard->stats.ndehugifies - base_dehugifies;
	expect_u64_le(hugifies, 4,
	    "16 alloc/free cycles caused %" FMTu64 " hugifications; the "
	    "pageslab is being rebuilt rather than reused", hugifies);
	expect_u64_eq(dehugifies, 0,
	    "%" FMTu64 " dehugifications; the pageslab is being handed back "
	    "and re-hugified rather than held", dehugifies);
	/*
	 * And the pageslab has to still be huge at the end.  Without this the
	 * case passes trivially against an implementation that ignores the
	 * per-pool overrides: nothing hugifies, so nothing thrashes.
	 */
	expect_zu_ge(shard->psset.stats.slabs[1].npageslabs, 1,
	    "the band holds no hugified pageslab, so there was nothing that "
	    "could have thrashed");
}
TEST_END

TEST_BEGIN(test_set_opt_rejects) {
	/*
	 * The parser side, driven directly so the malformed cases do not have
	 * to be smuggled through MALLOC_CONF one process at a time.
	 */
	hpa_pool_layout_t layout;
	hpa_pool_layout_init(&layout);
	expect_false(hpa_pool_layout_add(&layout, PAGE, 4 * PAGE, 1), "");
	expect_false(hpa_pool_layout_add(&layout, 4 * PAGE + 1, HUGEPAGE, 1),
	    "");

	/* A band that is not configured. */
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 2 * PAGE,
	                "hugify_delay_ms", sizeof("hugify_delay_ms") - 1, "0",
	                1),
	    "a range that names no band should be rejected");

	/* A band whose start is right but whose end is not. */
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, HUGEPAGE,
	                "hugify_delay_ms", sizeof("hugify_delay_ms") - 1, "0",
	                1),
	    "a partially matching range should be rejected");

	/* An option that does not exist. */
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE, "nonsense",
	                sizeof("nonsense") - 1, "1", 1),
	    "an unknown option should be rejected");

	/* A prefix of a real option, which strncmp alone would accept. */
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE, "hugify",
	                sizeof("hugify") - 1, "0", 1),
	    "a prefix of an option name should be rejected");

	/* Values out of range, and values of the wrong shape. */
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                "slab_max_alloc", sizeof("slab_max_alloc") - 1, "1", 1),
	    "a slab_max_alloc below PAGE should be rejected");
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                "hugification_threshold",
	                sizeof("hugification_threshold") - 1, "99999999", 8),
	    "a threshold above HUGEPAGE should be rejected");
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                "hugify_sync", sizeof("hugify_sync") - 1, "yes", 3),
	    "a non-boolean hugify_sync should be rejected");
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                "hugify_style", sizeof("hugify_style") - 1, "e", 1),
	    "a prefix of a hugify_style name should be rejected");
	expect_true(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                "hugify_delay_ms", sizeof("hugify_delay_ms") - 1, "12x",
	                3),
	    "trailing junk after a number should be rejected");

	/* And the accepting cases, so the rejections above mean something. */
	expect_false(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                 "hugify_style", sizeof("hugify_style") - 1, "eager",
	                 5),
	    "a valid hugify_style should be accepted");
	expect_d_eq((int)layout.pools[0].opts.hugify_style,
	    (int)hpa_hugify_style_eager, "hugify_style did not take");
	expect_false(hpa_pool_layout_set_opt(&layout, PAGE, 4 * PAGE,
	                 "dirty_mult", sizeof("dirty_mult") - 1, "-1", 2),
	    "dirty_mult:-1 should be accepted");
	expect_u32_eq((uint32_t)layout.pools[0].opts.dirty_mult, (uint32_t)-1,
	    "dirty_mult:-1 did not take");
	expect_false(hpa_pool_layout_set_opt(&layout, 4 * PAGE + 1, HUGEPAGE,
	                 "sec_nshards", sizeof("sec_nshards") - 1, "0", 1),
	    "sec_nshards should be accepted");
	expect_zu_eq(layout.pools[1].sec_opts.nshards, 0,
	    "sec_nshards did not take");
}
TEST_END

int
main(void) {
	return test(test_set_opt_rejects, test_overrides_reach_the_shards,
	    test_pairs_fill_one_pageslab, test_lowered_threshold_hugifies,
	    test_singletons_do_not_thrash);
}
