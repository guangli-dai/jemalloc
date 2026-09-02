#include "test/jemalloc_test.h"

#include "test/hpa_pool.h"

/*
 * These tests exercise the routing logic in isolation: no shards are
 * constructed, so no hugepages are mapped.  Everything below builds a pool set
 * by hand, fills in only the fields the router reads, and checks the two
 * decisions the design makes -- size to pool, then pool to shard.
 *
 * Band bounds are written as multiples of PAGE rather than in bytes, so that
 * the same tests mean the same thing on a 64 KiB-page build, where a literal
 * 16 KiB would be smaller than a page and rejected outright.  A multiple of
 * PAGE is also always an exact page-size class, which is what makes the
 * boundary assertions below exact rather than approximate.
 */
#define POOL_SMALL_MAX (4 * PAGE)  /* 16 KiB with the default page size. */
#define POOL_MED_MAX   (16 * PAGE) /* 64 KiB with the default page size. */

/*
 * A real array, so that the pointer arithmetic the tests do to recover a shard
 * index is defined behaviour.  Nothing is ever dereferenced; only the addresses
 * matter.  hpa_shard_t is large, so keep this small and size the layouts to
 * fit.
 */
#define TEST_MAX_SHARDS 16
static hpa_shard_t test_shards[TEST_MAX_SHARDS];

static void
pool_set_init(hpa_pool_set_t *set, unsigned npools, const size_t *bounds,
    const unsigned *nshards, hpa_pool_pick_t pick) {
	memset(set, 0, sizeof(*set));
	set->npools = npools;
	size_t   size_min = 1;
	unsigned next_shard = 0;
	for (unsigned i = 0; i < npools; i++) {
		set->pools[i].size_min = size_min;
		set->pools[i].size_max = bounds[i];
		set->pools[i].first_shard = next_shard;
		set->pools[i].nshards = nshards[i];
		set->pools[i].pick = pick;
		atomic_store_u(&set->pools[i].rr_next, 0, ATOMIC_RELAXED);
		size_min = bounds[i] + 1;
		next_shard += nshards[i];
	}
	assert_u_le(next_shard, TEST_MAX_SHARDS, "test layout too large");
	set->nshards_total = next_shard;
	set->shards = test_shards;
	/*
	 * Build the table with the production code, not a copy of it.  An
	 * earlier version of this file reimplemented the same first-match loop
	 * here, which meant the table tests below compared the test's answer
	 * against the test's own answer and would have passed no matter what
	 * hpa_pool_build_route_table() did.
	 */
	hpa_pool_build_route_table(set);
	set->initialized = true;
}

/*
 * Which shard index within the whole set did routing pick?
 *
 * szind is derived from the requested size the way the allocator derives it,
 * because that -- not the extent size -- is what the router keys on.
 */
static unsigned
route_to_id(hpa_pool_set_t *set, size_t size, unsigned hint) {
	hpa_shard_t *shard = hpa_route(set, size, /* slab */ false,
	    sz_size2index(size), hint);
	return (unsigned)(shard - set->shards);
}

/*
 * Largest class the router can be handed -- mirrors the bound in
 * hpa_pool_build_route_table(), including the narrower domain sz_index2size()
 * has when large size classes are disabled.
 */
static szind_t
max_routed_index(void) {
	szind_t max = sz_size2index(HUGEPAGE);
	if (sz_large_size_classes_disabled()) {
		szind_t capped = sz_size2index(USIZE_GROW_SLOW_THRESHOLD);
		if (capped < max) {
			max = capped;
		}
	}
	return max;
}

/* The pool a requested size routes to. */
static hpa_pool_t *
pool_of_size(hpa_pool_set_t *set, size_t size) {
	return hpa_pool_lookup(set,
	    hpa_route_key(size, /* slab */ false, sz_size2index(size)));
}

TEST_BEGIN(test_route_table_totality) {
	/*
	 * Every size the HPA can be asked for must map to exactly one pool.  A
	 * hole would not crash: pa_alloc() would quietly fall through to the
	 * PAC, which is correctness-preserving and silently mistuned -- the
	 * kind of bug that hides for a long time.  So check the whole domain.
	 */
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	for (szind_t i = 0; i <= max_routed_index(); i++) {
		size_t size = sz_index2size(i);
		if (size > HUGEPAGE) {
			/* Never routed: pa_alloc() gates on the extent size. */
			continue;
		}
		hpa_pool_t *pool = pool_of_size(&set, size);
		expect_ptr_not_null(pool, "size %zu routed nowhere", size);
		expect_true(size <= pool->size_max,
		    "size %zu routed to a pool whose band ends at %zu", size,
		    pool->size_max);
		expect_true(size >= pool->size_min,
		    "size %zu routed to a pool whose band starts at %zu", size,
		    pool->size_min);
	}
}
TEST_END

TEST_BEGIN(test_route_table_matches_linear_scan) {
	/*
	 * The table is a precomputed shortcut for "first pool whose band
	 * reaches this size".  Check the shortcut against the definition at
	 * every page-size class, not just at the boundaries.
	 */
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	for (szind_t k = 0; k <= max_routed_index(); k++) {
		size_t size = sz_index2size(k);
		if (size > HUGEPAGE) {
			continue;
		}
		hpa_pool_t *want = NULL;
		for (unsigned i = 0; i < set.npools; i++) {
			if (size <= set.pools[i].size_max) {
				want = &set.pools[i];
				break;
			}
		}
		hpa_pool_t *got = pool_of_size(&set, size);
		expect_ptr_eq(got, want,
		    "size %zu: table and linear scan disagree", size);
	}
}
TEST_END

TEST_BEGIN(test_route_is_monotonic) {
	/*
	 * Pools are size bands, so routing must never go backwards as size
	 * grows.  A non-monotonic table would mean two sizes interleaving
	 * across pools, which defeats the segregation the whole design is for.
	 */
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	hpa_pool_t *prev = pool_of_size(&set, sz_index2size(0));
	for (szind_t k = 0; k <= max_routed_index(); k++) {
		size_t size = sz_index2size(k);
		if (size > HUGEPAGE) {
			continue;
		}
		hpa_pool_t *pool = pool_of_size(&set, size);
		expect_true(pool >= prev,
		    "routing went backwards at size %zu", size);
		prev = pool;
	}
}
TEST_END

TEST_BEGIN(test_route_boundaries_are_exact) {
	/* The interesting boundaries are size-class boundaries, so they land
	 * exactly: 16 KiB belongs to pool 0, the next class up to pool 1. */
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	expect_ptr_eq(hpa_pool_lookup(&set,
	    hpa_route_key(POOL_SMALL_MAX, false, sz_size2index(POOL_SMALL_MAX))), &set.pools[0],
	    "the small bound should be the top of pool 0");
	expect_ptr_eq(hpa_pool_lookup(&set,
	    hpa_route_key(POOL_SMALL_MAX + 1, false, sz_size2index(POOL_SMALL_MAX + 1))),
	    &set.pools[1], "the class above the small bound should be pool 1");
	expect_ptr_eq(hpa_pool_lookup(&set,
	    hpa_route_key(POOL_MED_MAX, false, sz_size2index(POOL_MED_MAX))), &set.pools[1],
	    "the medium bound should be the top of pool 1");
	expect_ptr_eq(hpa_pool_lookup(&set,
	    hpa_route_key(POOL_MED_MAX + 1, false, sz_size2index(POOL_MED_MAX + 1))),
	    &set.pools[2], "the class above the medium bound should be pool 2");
	expect_ptr_eq(hpa_pool_lookup(&set,
	    hpa_route_key(HUGEPAGE, false, sz_size2index(HUGEPAGE))), &set.pools[2],
	    "HUGEPAGE should be the top of the last pool");
}
TEST_END

TEST_BEGIN(test_pick_stays_within_pool) {
	/*
	 * Step 2's freedom is bounded: a pool may pick any of its own shards
	 * and none of anyone else's.  Straying would hand an extent to a shard
	 * in the wrong size band, quietly undoing segregation.
	 */
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};

	for (int p = 0; p < 2; p++) {
		hpa_pool_pick_t pick = (p == 0) ? hpa_pool_pick_arena
		                                : hpa_pool_pick_roundrobin;
		pool_set_init(&set, 3, bounds, nshards, pick);
		for (unsigned i = 0; i < set.npools; i++) {
			hpa_pool_t *pool = &set.pools[i];
			for (unsigned hint = 0; hint < 64; hint++) {
				hpa_shard_t *shard = hpa_pool_pick_shard(&set,
				    pool, hint);
				unsigned id = (unsigned)(shard - set.shards);
				expect_true(id >= pool->first_shard
				        && id < pool->first_shard
				            + pool->nshards,
				    "picker %d: pool %u handed out shard %u, "
				    "outside [%u, %u)", p, i, id,
				    pool->first_shard,
				    pool->first_shard + pool->nshards);
			}
		}
	}
}
TEST_END

TEST_BEGIN(test_pick_arena_is_identity) {
	/*
	 * The identity layout is the control arm for the whole project: one
	 * pool, one shard per arena, picked by arena index.  Arena i must
	 * always get shard i, or the "behaviour is unchanged" claim that
	 * S1-S3 rest on is not true.
	 */
	hpa_pool_set_t    set;
	hpa_pool_layout_t layout;
	unsigned          narenas = 10;
	hpa_pool_layout_identity(&layout, narenas, HPA_MAX_SHARDS_TOTAL);

	expect_u_eq(layout.npools, 1, "identity layout should have one pool");
	expect_zu_eq(layout.pools[0].size_max, HUGEPAGE,
	    "identity pool should span everything");
	expect_u_eq(layout.pools[0].nshards, narenas,
	    "identity layout should have one shard per arena");
	expect_d_eq((int)layout.pick, (int)hpa_pool_pick_arena,
	    "identity layout should pick by arena");

	size_t   bounds[] = {HUGEPAGE};
	unsigned nshards[] = {narenas};
	pool_set_init(&set, 1, bounds, nshards, hpa_pool_pick_arena);

	for (unsigned arena = 0; arena < narenas; arena++) {
		for (size_t size = 8; size <= HUGEPAGE; size *= 2) {
			expect_u_eq(route_to_id(&set, size, arena), arena,
			    "arena %u, size %zu: identity layout must route to "
			    "shard %u", arena, size, arena);
		}
	}
}
TEST_END

TEST_BEGIN(test_pick_roundrobin_spreads) {
	/*
	 * Round robin should ignore the hint and cycle.  This is the first
	 * picker that can make the owning shard differ from the requesting
	 * arena, which is what the S4 reroute test will lean on.
	 */
	hpa_pool_set_t set;
	size_t         bounds[] = {HUGEPAGE};
	unsigned       nshards[] = {4};
	pool_set_init(&set, 1, bounds, nshards, hpa_pool_pick_roundrobin);

	unsigned counts[4] = {0};
	for (unsigned i = 0; i < 4 * 25; i++) {
		/* Constant hint: any spreading is the picker's doing. */
		unsigned id = route_to_id(&set, PAGE, /* hint */ 7);
		expect_u_lt(id, 4, "shard id out of range");
		counts[id]++;
	}
	for (unsigned i = 0; i < 4; i++) {
		expect_u_eq(counts[i], 25,
		    "round robin should have hit shard %u exactly 25 times",
		    i);
	}
}
TEST_END

TEST_BEGIN(test_single_shard_pool) {
	/* A one-shard pool is the degenerate case the big band may well want;
	 * both pickers must collapse to it without dividing by zero. */
	hpa_pool_set_t set;
	size_t         bounds[] = {HUGEPAGE};
	unsigned       nshards[] = {1};

	for (int p = 0; p < 2; p++) {
		hpa_pool_pick_t pick = (p == 0) ? hpa_pool_pick_arena
		                                : hpa_pool_pick_roundrobin;
		pool_set_init(&set, 1, bounds, nshards, pick);
		for (unsigned hint = 0; hint < 8; hint++) {
			expect_u_eq(route_to_id(&set, PAGE, hint), 0,
			    "picker %d: a single-shard pool must always pick "
			    "shard 0", p);
		}
	}
}
TEST_END

TEST_BEGIN(test_layout_clamp_respects_the_limit) {
	/*
	 * Every shard id has to fit in the extent's owner field, so the clamp
	 * is not advisory -- exceeding it truncates the recorded owner in a
	 * release build and routes frees to the wrong shard.
	 *
	 * The shape that breaks naive rounding is one huge pool beside several
	 * tiny ones: flooring each proportional share and then lifting the
	 * zeroes back up to one adds nearly a whole shard per lift, which the
	 * floors did not save.  That lands on 4102 against a limit of 4096.
	 */
	struct {
		const char *name;
		unsigned    nshards_max;
		unsigned    npools;
		unsigned    nshards[HPA_MAX_POOLS];
	} cases[] = {
	    {"under the limit, untouched", HPA_MAX_SHARDS_TOTAL, 3, {4, 2, 1}},
	    {"uniform, over the limit", HPA_MAX_SHARDS_TOTAL, 8,
	        {4095, 4095, 4095, 4095, 4095, 4095, 4095, 4095}},
	    {"one huge beside seven tiny", HPA_MAX_SHARDS_TOTAL, 8,
	        {1, 1, 1, 1, 1, 1, 1, 4095}},
	    {"one huge beside seven tiny, far over", HPA_MAX_SHARDS_TOTAL, 8,
	        {1, 1, 1, 1, 1, 1, 1, 4096}},
	    {"single pool at the limit", HPA_MAX_SHARDS_TOTAL, 1, {4096}},
	    /*
	     * The tunable limit is the one that actually bites in practice:
	     * opt_hpa_pool_nshards_max defaults to 64, far below the field
	     * width, so these are the shapes a real configuration hits.
	     */
	    {"tunable limit, identity-shaped", 64, 1, {704}},
	    {"tunable limit, one huge beside seven tiny", 64, 8,
	        {1, 1, 1, 1, 1, 1, 1, 704}},
	    /* The tightest legal cap: exactly one shard per pool. */
	    {"tunable limit equal to npools", 4, 4, {8, 8, 8, 8}},
	};

	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_pool_layout_t layout;
		hpa_pool_layout_init(&layout);
		layout.nshards_max = cases[c].nshards_max;
		layout.npools = cases[c].npools;
		unsigned before = 0;
		for (unsigned i = 0; i < layout.npools; i++) {
			layout.pools[i].nshards = cases[c].nshards[i];
			layout.pools[i].size_max = HUGEPAGE;
			before += cases[c].nshards[i];
		}

		hpa_pool_layout_clamp(&layout);

		unsigned after = 0;
		for (unsigned i = 0; i < layout.npools; i++) {
			expect_u_gt(layout.pools[i].nshards, 0,
			    "%s: pool %u was clamped out of existence",
			    cases[c].name, i);
			after += layout.pools[i].nshards;
		}
		expect_u_le(after, cases[c].nshards_max,
		    "%s: clamped to %u shards, past the %u limit",
		    cases[c].name, after, cases[c].nshards_max);
		if (before <= cases[c].nshards_max) {
			expect_u_eq(after, before,
			    "%s: a layout within the limit should be left "
			    "alone", cases[c].name);
		}
	}
}
TEST_END

TEST_BEGIN(test_layout_add_tiles_or_rejects) {
	/*
	 * hpa_pool_layout_add() is what the MALLOC_CONF parser drives, one
	 * band at a time.  Its whole job is to make a malformed band fail
	 * where it is written: bands must tile [PAGE, HUGEPAGE] exactly, and a
	 * gap is the dangerous case, because sizes in the gap would route to
	 * whichever pool inherited them (or, if the gap is at the top, fall
	 * through to the PAC) with nothing failing to say so.
	 */
	hpa_pool_layout_t layout;

	/* A well-formed three-band tiling. */
	hpa_pool_layout_init(&layout);
	expect_false(hpa_pool_layout_add(&layout, 1, POOL_SMALL_MAX, 4),
	    "first band should be accepted");
	expect_false(hpa_pool_layout_add(&layout, POOL_SMALL_MAX + 1,
	                 POOL_MED_MAX, 2),
	    "abutting band should be accepted");
	expect_false(
	    hpa_pool_layout_add(&layout, POOL_MED_MAX + 1, HUGEPAGE, 1),
	    "final band should be accepted");
	expect_u_eq(layout.npools, 3, "should have built three pools");
	expect_false(hpa_pool_layout_validate(&layout),
	    "a tiling of [PAGE, HUGEPAGE] should validate");

	/* First band must start at PAGE. */
	hpa_pool_layout_init(&layout);
	expect_true(hpa_pool_layout_add(&layout, 2, HUGEPAGE, 1),
	    "a first band starting above 1 should be rejected");

	/* Gap between bands. */
	hpa_pool_layout_init(&layout);
	expect_false(hpa_pool_layout_add(&layout, 1, POOL_SMALL_MAX, 1),
	    "first band should be accepted");
	expect_true(hpa_pool_layout_add(&layout, POOL_SMALL_MAX + 2,
	                POOL_MED_MAX, 1),
	    "a band leaving a gap should be rejected");

	/* Overlap with the previous band. */
	hpa_pool_layout_init(&layout);
	expect_false(hpa_pool_layout_add(&layout, 1, POOL_MED_MAX, 1),
	    "first band should be accepted");
	expect_true(
	    hpa_pool_layout_add(&layout, POOL_SMALL_MAX, HUGEPAGE, 1),
	    "an overlapping band should be rejected");

	/* Backwards band. */
	hpa_pool_layout_init(&layout);
	expect_true(hpa_pool_layout_add(&layout, 1, 0, 1),
	    "a band whose end precedes its start should be rejected");

	/*
	 * More bands than the set can hold.  Each band here is one page wide
	 * and abuts the last, so the only thing wrong with the final add is
	 * that there is nowhere to put it.
	 */
	hpa_pool_layout_init(&layout);
	for (unsigned i = 0; i < HPA_MAX_POOLS; i++) {
		size_t start = (i == 0) ? 1 : i * PAGE + 1;
		expect_false(
		    hpa_pool_layout_add(&layout, start, (i + 1) * PAGE, 1),
		    "band %u should fit", i);
	}
	expect_true(hpa_pool_layout_add(&layout, HPA_MAX_POOLS * PAGE + 1,
	                HUGEPAGE, 1),
	    "a band past HPA_MAX_POOLS should be rejected");
}
TEST_END

TEST_BEGIN(test_layout_validate_rejects) {
	/*
	 * What add() cannot catch on its own: properties of the layout as a
	 * whole.  These are the configurations that would boot into a router
	 * that is quietly wrong rather than one that refuses.
	 */
	hpa_pool_layout_t layout;

	/* Does not reach HUGEPAGE: the top of the range has no pool. */
	hpa_pool_layout_init(&layout);
	expect_false(
	    hpa_pool_layout_add(&layout, 1, HUGEPAGE / 2, 1), "");
	expect_true(hpa_pool_layout_validate(&layout),
	    "a layout that stops short of HUGEPAGE should be rejected");

	/* Past HUGEPAGE: describes extents the HPA will never serve. */
	hpa_pool_layout_init(&layout);
	expect_false(
	    hpa_pool_layout_add(&layout, 1, 2 * HUGEPAGE, 1), "");
	expect_true(hpa_pool_layout_validate(&layout),
	    "a band above HUGEPAGE should be rejected");

	/* Not page-aligned. */
	hpa_pool_layout_init(&layout);
	expect_false(
	    hpa_pool_layout_add(&layout, 1, HUGEPAGE - 1, 1), "");
	expect_true(hpa_pool_layout_validate(&layout),
	    "an unaligned band bound should be rejected");

	/* A pool with no shards. */
	hpa_pool_layout_init(&layout);
	expect_false(hpa_pool_layout_add(&layout, 1, HUGEPAGE, 0), "");
	expect_true(hpa_pool_layout_validate(&layout),
	    "a pool with no shards should be rejected");

	/* No pools at all. */
	hpa_pool_layout_init(&layout);
	expect_true(hpa_pool_layout_validate(&layout),
	    "an empty layout should be rejected");

	/*
	 * A cap too small to give every pool a shard.  Clamping instead would
	 * have to delete a pool, which silently rewrites the routing the
	 * caller asked for.
	 */
	hpa_pool_layout_init(&layout);
	expect_false(
	    hpa_pool_layout_add(&layout, 1, POOL_SMALL_MAX, 1), "");
	expect_false(
	    hpa_pool_layout_add(&layout, POOL_SMALL_MAX + 1, HUGEPAGE, 1), "");
	layout.nshards_max = 1;
	expect_true(hpa_pool_layout_validate(&layout),
	    "nshards_max below npools should be rejected");
	layout.nshards_max = 2;
	expect_false(hpa_pool_layout_validate(&layout),
	    "nshards_max equal to npools is the tightest legal cap");

	/* A cap above what an extent's owner field can address. */
	layout.nshards_max = HPA_MAX_SHARDS_TOTAL + 1;
	expect_true(hpa_pool_layout_validate(&layout),
	    "nshards_max above the owner field width should be rejected");
}
TEST_END

TEST_BEGIN(test_layout_identity_is_one_band) {
	/*
	 * The rollback switch selects this, so it has to be exactly the
	 * pre-pool topology: one band over everything, one shard per arena,
	 * picked by arena index.  The cap is applied by the clamp rather than
	 * here, so that "asked for" and "got" stay distinguishable.
	 */
	hpa_pool_layout_t layout;
	hpa_pool_layout_identity(&layout, 8, HPA_POOL_NSHARDS_MAX_DEFAULT);
	expect_u_eq(layout.npools, 1, "identity should be a single band");
	expect_zu_eq(layout.pools[0].size_max, HUGEPAGE, "band should span all");
	expect_u_eq(layout.pools[0].nshards, 8, "one shard per arena");
	expect_d_eq((int)layout.pick, (int)hpa_pool_pick_arena, "by arena");
	expect_u_eq(layout.nshards_max, HPA_POOL_NSHARDS_MAX_DEFAULT,
	    "identity should carry the cap it was given");
	expect_false(hpa_pool_layout_validate(&layout),
	    "the identity layout must always validate");

	/*
	 * Above the cap, arenas share shards -- and identity applies the cap
	 * itself rather than leaving it to the clamp, because narenas is not
	 * a number anyone asked for and the clamp announces itself on stderr.
	 */
	hpa_pool_layout_identity(&layout, 704, HPA_POOL_NSHARDS_MAX_DEFAULT);
	expect_u_eq(layout.pools[0].nshards, HPA_POOL_NSHARDS_MAX_DEFAULT,
	    "the cap should be what bounds a large narenas");
	hpa_pool_layout_clamp(&layout);
	expect_u_eq(layout.pools[0].nshards, HPA_POOL_NSHARDS_MAX_DEFAULT,
	    "the clamp should have nothing left to do");
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_layout_clamp_respects_the_limit,
	    test_layout_add_tiles_or_rejects, test_layout_validate_rejects,
	    test_layout_identity_is_one_band,
	    test_route_table_totality,
	    test_route_table_matches_linear_scan, test_route_is_monotonic,
	    test_route_boundaries_are_exact, test_pick_stays_within_pool,
	    test_pick_arena_is_identity, test_pick_roundrobin_spreads,
	    test_single_shard_pool);
}
