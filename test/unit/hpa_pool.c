#include "test/jemalloc_test.h"

#include "test/hpa_pool.h"

/*
 * These tests exercise the routing logic in isolation: no shards are
 * constructed, so no hugepages are mapped.  Everything below builds a pool set
 * by hand, fills in only the fields the router reads, and checks the two
 * decisions the design makes -- size to pool, then pool to shard.
 *
 * Band bounds are PAGE multiples so the tests use allocation size classes
 * available across page-size configurations.
 */
#define POOL_SMALL_MAX (4 * PAGE)  /* 16 KiB with the default page size. */
#define POOL_MED_MAX   (16 * PAGE) /* 64 KiB with the default page size. */

/* Real objects used only to give the test slots distinct shard pointers. */
#define TEST_MAX_SHARDS 16
static hpa_shard_t test_shards[TEST_MAX_SHARDS];

static void
pool_set_init(hpa_pool_set_t *set, unsigned npools, const size_t *bounds,
    const unsigned *nshards, hpa_pool_pick_t pick) {
	memset(set, 0, sizeof(*set));
	set->npools = npools;
	unsigned next_shard = 0;
	for (unsigned i = 0; i < npools; i++) {
		set->pools[i].size_max = bounds[i];
		set->pools[i].szind_max = sz_size2index(bounds[i]);
		set->pools[i].pool_ind = i;
		set->pools[i].nshards = nshards[i];
		set->pools[i].pick = pick;
		atomic_store_u(&set->pools[i].rr_next, 0, ATOMIC_RELAXED);
		for (unsigned j = 0; j < nshards[i]; j++) {
			atomic_store_p(&set->pools[i].shards[j],
			    &test_shards[next_shard + j], ATOMIC_RELAXED);
		}
		next_shard += nshards[i];
	}
	assert_u_le(next_shard, TEST_MAX_SHARDS, "test configuration too large");
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
	hpa_shard_t *shard = hpa_route(TSDN_NULL, set, size, /* slab */ false,
	    sz_size2index(size), hint);
	return (unsigned)(shard - test_shards);
}

/*
 * Largest class the router can be handed -- mirrors the bound in
 * routing accepts, including the narrower domain sz_index2size() has when
 * large size classes are disabled.
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

TEST_BEGIN(test_route_lookup_totality) {
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
	}
}
TEST_END

TEST_BEGIN(test_route_lookup_matches_linear_scan) {
	/*
	 * Check lookup against the configured bounds at every allocation size
	 * class, not just at the boundaries.
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
		    "size %zu: lookup and configured bounds disagree", size);
	}
}
TEST_END

TEST_BEGIN(test_route_is_monotonic) {
	/*
	 * Pools are size bands, so routing must never go backwards as size
	 * grows.  Non-monotonic routing would mean two sizes interleaving
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

TEST_BEGIN(test_route_uses_allocation_size_for_slabs) {
	hpa_pool_set_t set;
	size_t bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	/* Different slab extents must not change an allocation's route. */
	szind_t szind = sz_size2index(64);
	unsigned key_small_slab = hpa_route_key(PAGE, true, szind);
	unsigned key_large_slab = hpa_route_key(POOL_MED_MAX, true, szind);
	expect_u_eq(key_small_slab, key_large_slab,
	    "slab extent size must not affect the routing key");
	expect_ptr_eq(hpa_pool_lookup(&set, key_small_slab), &set.pools[0],
	    "64-byte allocations should use the small allocation pool");
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
				hpa_shard_t *shard = hpa_pool_pick_shard(TSDN_NULL,
				    pool, hint);
				bool found = false;
				for (unsigned j = 0; j < pool->nshards; j++) {
					found |= shard == atomic_load_p(
					    &pool->shards[j], ATOMIC_RELAXED);
				}
				expect_true(found,
				    "picker %d returned a shard outside pool %u", p,
				    i);
			}
		}
	}
}
TEST_END

TEST_BEGIN(test_pool_local_shard_ids) {
	hpa_pool_set_t set;
	size_t         bounds[] = {POOL_SMALL_MAX, POOL_MED_MAX, HUGEPAGE};
	unsigned       nshards[] = {4, 2, 1};
	pool_set_init(&set, 3, bounds, nshards, hpa_pool_pick_arena);

	for (unsigned i = 0; i < set.npools; i++) {
		hpa_pool_t *pool = &set.pools[i];
		for (unsigned j = 0; j < pool->nshards; j++) {
			unsigned id = hpa_pool_shard_id(pool, j);
			expect_u_eq(id,
			    i * HPA_MAX_SHARDS_PER_POOL + j,
			    "unexpected stable shard id");
			expect_ptr_eq(hpa_pool_shard_from_id(&set, id),
			    atomic_load_p(&pool->shards[j], ATOMIC_RELAXED),
			    "owner id resolved to the wrong pool-local shard");
		}
	}
	expect_ptr_null(hpa_pool_shard_from_id(&set,
	    HPA_MAX_POOLS * HPA_MAX_SHARDS_PER_POOL),
	    "an out-of-range owner id should not be resolved");
	expect_ptr_null(hpa_pool_shard_from_id(&set,
	    hpa_pool_shard_id(&set.pools[1], set.pools[1].nshards)),
	    "an unused pool-local slot should not be resolved");
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
	hpa_pool_configs_t configs;
	unsigned          narenas = 10;
	expect_false(hpa_pool_configs_identity(&configs, narenas,
	    HPA_MAX_SHARDS_PER_POOL),
	    "identity configuration should fit");

	expect_u_eq(configs.npools, 1, "identity configs should have one pool");
	expect_zu_eq(configs.pools[0].size_max, HUGEPAGE,
	    "identity pool should span everything");
	expect_u_eq(configs.pools[0].nshards, narenas,
	    "identity configs should have one shard per arena");
	expect_d_eq((int)configs.pick, (int)hpa_pool_pick_arena,
	    "identity configs should pick by arena");

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

TEST_BEGIN(test_identity_caps_many_arenas) {
	hpa_pool_configs_t configs;
	expect_false(hpa_pool_configs_identity(&configs,
	    HPA_MAX_SHARDS_PER_POOL + 1, HPA_POOL_NSHARDS_MAX_DEFAULT),
	    "the configured identity cap should be valid");
	expect_u_eq(configs.pools[0].nshards,
	    HPA_POOL_NSHARDS_MAX_DEFAULT,
	    "identity configuration should apply its explicit cap");
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

TEST_BEGIN(test_pick_roundrobin_wraps) {
	hpa_pool_set_t set;
	size_t bounds[] = {HUGEPAGE};
	unsigned nshards[] = {3};
	pool_set_init(&set, 1, bounds, nshards, hpa_pool_pick_roundrobin);
	atomic_store_u(&set.pools[0].rr_next, UINT_MAX, ATOMIC_RELAXED);

	expect_u_eq(route_to_id(&set, PAGE, 0), UINT_MAX % 3,
	    "round robin should select correctly before counter wrap");
	expect_u_eq(route_to_id(&set, PAGE, 0), 0,
	    "round robin should select correctly after counter wrap");
}
TEST_END

TEST_BEGIN(test_configs_validate) {
	hpa_pool_configs_t configs;
	memset(&configs, 0, sizeof(configs));
	configs.nshards_max = HPA_MAX_SHARDS_TOTAL;
	configs.npools = 2;
	configs.pick = hpa_pool_pick_arena;
	configs.pools[0].size_max = POOL_SMALL_MAX;
	configs.pools[0].nshards = 2;
	configs.pools[1].size_max = HUGEPAGE;
	configs.pools[1].nshards = 1;
	expect_false(hpa_pool_configs_validate(&configs),
	    "valid configuration rejected");

	configs.pools[0].size_max++;
	expect_true(hpa_pool_configs_validate(&configs),
	    "non-size-class boundary accepted");
	configs.pools[0].size_max = POOL_SMALL_MAX;
	configs.pools[1].nshards = 0;
	expect_true(hpa_pool_configs_validate(&configs),
	    "pool without shards accepted");
	configs.pools[1].nshards = 1;
	configs.pools[1].size_max = POOL_MED_MAX;
	expect_true(hpa_pool_configs_validate(&configs),
	    "configuration that does not cover HUGEPAGE accepted");
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
	 * The shape that breaks naive rounding is one large pool beside several
	 * tiny ones: flooring each proportional share and then lifting the
	 * zeroes back up to one adds nearly a whole shard per lift, which the
	 * floors did not save.  That lands on 4102 against a limit of 4096.
	 */
	struct {
		const char *name;
		unsigned    npools;
		unsigned    nshards[HPA_MAX_POOLS];
	} cases[] = {
	    {"under the limit, untouched", 3, {4, 2, 1}},
	    {"uniform, over the limit", 8,
	        {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024}},
	    {"skewed, over the limit", 8,
	        {1024, 1024, 1024, 1024, 1024, 1, 1, 1}},
	    {"single pool at its slot limit", 1,
	        {HPA_MAX_SHARDS_PER_POOL}},
	};

	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_pool_configs_t configs;
		memset(&configs, 0, sizeof(configs));
		configs.nshards_max = HPA_MAX_SHARDS_TOTAL;
		configs.npools = cases[c].npools;
		unsigned before = 0;
		for (unsigned i = 0; i < configs.npools; i++) {
			configs.pools[i].nshards = cases[c].nshards[i];
			configs.pools[i].size_max = HUGEPAGE;
			before += cases[c].nshards[i];
		}

		hpa_pool_configs_clamp(&configs);

		unsigned after = 0;
		for (unsigned i = 0; i < configs.npools; i++) {
			expect_u_gt(configs.pools[i].nshards, 0,
			    "%s: pool %u was clamped out of existence",
			    cases[c].name, i);
			after += configs.pools[i].nshards;
		}
		expect_u_le(after, HPA_MAX_SHARDS_TOTAL,
		    "%s: clamped to %u shards, past the %u limit",
		    cases[c].name, after, (unsigned)HPA_MAX_SHARDS_TOTAL);
		if (before <= HPA_MAX_SHARDS_TOTAL) {
			expect_u_eq(after, before,
			    "%s: a layout within the limit should be left "
			    "alone", cases[c].name);
		}
	}
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_layout_clamp_respects_the_limit,
	    test_configs_validate,
	    test_route_lookup_totality,
	    test_route_lookup_matches_linear_scan, test_route_is_monotonic,
	    test_route_boundaries_are_exact,
	    test_route_uses_allocation_size_for_slabs,
	    test_pick_stays_within_pool,
	    test_pool_local_shard_ids, test_pick_arena_is_identity,
	    test_identity_caps_many_arenas,
	    test_pick_roundrobin_spreads, test_pick_roundrobin_wraps,
	    test_single_shard_pool);
}
