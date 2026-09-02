#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa_pool.h"

/*
 * The point of the whole exercise, checked end to end: an extent's owning HPA
 * shard is not its arena.
 *
 * Everything through S3 kept the two numerically equal -- one pool, one shard
 * per arena, picked by arena index -- so nothing before now could tell whether
 * the allocator was reading the recorded owner or merely getting away with
 * reading the arena index.  A multi-band layout with a round-robin picker
 * drives them apart, and then every free has to find its way home by the
 * recorded owner alone.
 *
 * hpa_sec_nshards:0 keeps the SEC out of the way: with a cache in front of the
 * shards, most frees would never reach one and the test would mostly be
 * measuring the cache.
 */

/*
 * Band bounds have to be byte literals -- MALLOC_CONF is a string, and PAGE is
 * not a preprocessor constant -- so a configuration has to be written per page
 * geometry.  The two the project builds and tests are covered; any other
 * geometry skips rather than being handed a layout that cannot tile
 * [PAGE, HUGEPAGE].
 *
 * The three sizes are one per band, all above SC_LARGE_MINCLASS so they take
 * the large path and reach the HPA as the size asked for.
 */
#if LG_HUGEPAGE == 21 && LG_PAGE == 12
#define POOLS_CONF "hpa_pools:1-16384:4|16385-65536:4|65537-2097152:4,"
#define SIZE_BAND_0 16384
#define SIZE_BAND_1 65536
#define SIZE_BAND_2 524288
#elif LG_HUGEPAGE == 21 && LG_PAGE == 16
#define POOLS_CONF "hpa_pools:1-262144:4|262145-1048576:4|1048577-2097152:4,"
#define SIZE_BAND_0 262144
#define SIZE_BAND_1 524288
#define SIZE_BAND_2 1572864
#else
#define POOLS_CONF ""
#define SIZE_BAND_0 0
#define SIZE_BAND_1 0
#define SIZE_BAND_2 0
#endif

const char *malloc_conf = POOLS_CONF
    "hpa:true,"
    "hpa_shard_pools:true,"
    "hpa_pool_pick:roundrobin,"
    "hpa_pool_nshards_max:16,"
    "hpa_sec_nshards:0,"
    "hpa_dirty_mult:-1,"
    /*
     * Raise the HPA's own size ceiling past the largest band, so that the
     * band bounds are the only thing deciding where a size goes.
     */
    "hpa_slab_max_alloc:2097152,"
    /*
     * With cache-oblivious layout a large allocation's extent is a page
     * bigger than the request, which would quietly shift every test size
     * into the next band up.  Turning it off is what makes "this size is in
     * this band" mean what it says.
     */
    "cache_oblivious:false";

#define NPOOLS_EXPECTED 3

/*
 * Nothing below depends on the landing being what it looks like: every
 * assertion recomputes the pool from the extent size the router actually saw.
 */
static const size_t test_sizes[]
    = {SIZE_BAND_0, SIZE_BAND_1, SIZE_BAND_2};
#define NSIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

static bool
pools_configured(void) {
	return POOLS_CONF[0] != '\0' && hpa_supported() && opt_hpa
	    && !opt_cache_oblivious && hpa_pools_ready()
	    && hpa_pools_global.npools == NPOOLS_EXPECTED;
}

/* The pool an extent of this size belongs to, by the router's own rule. */
static hpa_pool_t *
pool_of(size_t size) {
	return hpa_pool_lookup(&hpa_pools_global,
	    hpa_route_key(size, /* slab */ false, sz_size2index(size)));
}

TEST_BEGIN(test_bands_segregate) {
	test_skip_if(!pools_configured());

	/*
	 * Step 1 is the forced half of routing: a size must always reach the
	 * same pool.  If it did not, extents of very different sizes could
	 * share a pageslab, which is exactly what segregation is for.
	 */
	size_t bounds[NPOOLS_EXPECTED];
	for (unsigned i = 0; i < NPOOLS_EXPECTED; i++) {
		bounds[i] = hpa_pools_global.pools[i].size_max;
	}
	expect_zu_eq(bounds[NPOOLS_EXPECTED - 1], HUGEPAGE,
	    "the last band must reach HUGEPAGE");

	for (size_t size = PAGE; size <= HUGEPAGE; size += PAGE) {
		hpa_pool_t *pool = pool_of(size);
		expect_true(size <= pool->size_max && size >= pool->size_min,
		    "size %zu routed to a band it does not belong to", size);
	}

	/* And no two bands may share a shard. */
	for (unsigned i = 0; i < hpa_pools_global.npools; i++) {
		for (unsigned j = i + 1; j < hpa_pools_global.npools; j++) {
			hpa_pool_t *a = &hpa_pools_global.pools[i];
			hpa_pool_t *b = &hpa_pools_global.pools[j];
			expect_true(
			    a->first_shard + a->nshards <= b->first_shard
			        || b->first_shard + b->nshards
			            <= a->first_shard,
			    "pools %u and %u share shards", i, j);
		}
	}
}
TEST_END

TEST_BEGIN(test_owner_is_not_the_arena) {
	test_skip_if(!pools_configured());

	tsdn_t  *tsdn = tsdn_fetch();
	unsigned arena_ind;
	size_t   sz = sizeof(arena_ind);
	expect_d_eq(mallctl("arenas.create", &arena_ind, &sz, NULL, 0), 0,
	    "arenas.create failed");
	int flags = MALLOCX_ARENA(arena_ind) | MALLOCX_TCACHE_NONE;

	/*
	 * Round robin ignores the hint, so consecutive allocations of the same
	 * size from the same arena must walk that band's shards.  Seeing more
	 * than one owner is what proves the owner is recorded rather than
	 * derived from the arena.
	 */
#define NALLOCS 8
	for (unsigned s = 0; s < NSIZES; s++) {
		size_t      size = test_sizes[s];
		void       *ptrs[NALLOCS];
		unsigned    owners[NALLOCS];
		unsigned    ndistinct = 0;
		bool        any_differs_from_arena = false;
		hpa_pool_t *pool = NULL;

		for (unsigned i = 0; i < NALLOCS; i++) {
			ptrs[i] = mallocx(size, flags);
			expect_ptr_not_null(ptrs[i], "mallocx(%zu) failed",
			    size);
			edata_t *edata = emap_edata_lookup(tsdn,
			    &arena_emap_global, ptrs[i]);
			expect_ptr_not_null(edata, "no edata for the extent");
			expect_d_eq((int)edata_pai_get(edata),
			    (int)EXTENT_PAI_HPA,
			    "size %zu did not come from the HPA", size);

			/*
			 * From the extent size, not the requested size: what
			 * the router keyed on is whatever the layers above
			 * rounded the request up to.
			 */
			hpa_pool_t *p = pool_of(edata_size_get(edata));
			if (pool == NULL) {
				pool = p;
			}
			expect_ptr_eq(p, pool,
			    "the same request size reached two different "
			    "bands");

			owners[i] = edata_hpa_shard_get(edata);
			expect_true(owners[i] >= pool->first_shard
			        && owners[i]
			            < pool->first_shard + pool->nshards,
			    "size %zu got shard %u, outside its band's "
			    "[%u, %u)", size, owners[i], pool->first_shard,
			    pool->first_shard + pool->nshards);
			/*
			 * The requesting arena is still recorded, and is still
			 * the arena: the two fields answer different questions
			 * and both have to keep answering them.
			 */
			expect_u_eq(edata_arena_ind_get(edata), arena_ind,
			    "the extent forgot which arena asked for it");
			if (owners[i] != arena_ind) {
				any_differs_from_arena = true;
			}

			bool seen = false;
			for (unsigned k = 0; k < i; k++) {
				if (owners[k] == owners[i]) {
					seen = true;
					break;
				}
			}
			if (!seen) {
				ndistinct++;
			}
		}

		if (pool->nshards > 1) {
			expect_u_gt(ndistinct, 1,
			    "size %zu: its band has %u shards but round robin "
			    "only ever used %u", size, pool->nshards,
			    ndistinct);
			expect_true(any_differs_from_arena,
			    "size %zu never routed away from the requesting "
			    "arena, so this proves nothing", size);
		}

		/*
		 * Free in an order unrelated to allocation order.  Each of
		 * these has to reach the shard recorded on the extent;
		 * reaching the arena's shard instead would corrupt a psset
		 * that never handed the extent out.
		 */
		for (unsigned i = 0; i < NALLOCS; i += 2) {
			dallocx(ptrs[i], flags);
		}
		for (unsigned i = 1; i < NALLOCS; i += 2) {
			dallocx(ptrs[i], flags);
		}
	}
#undef NALLOCS
}
TEST_END

TEST_BEGIN(test_free_from_a_different_arena) {
	test_skip_if(!pools_configured());

	/*
	 * The sharper version: allocate in one arena, free in another.  Under
	 * the old topology that was a cross-arena free and the extent still
	 * went back to the shard inside the arena recorded on it.  Now the
	 * arena has nothing to do with it, and only the recorded shard gets it
	 * home.
	 */
	unsigned a, b;
	size_t   sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", &a, &sz, NULL, 0), 0, "create a");
	sz = sizeof(unsigned);
	expect_d_eq(mallctl("arenas.create", &b, &sz, NULL, 0), 0, "create b");

	for (unsigned s = 0; s < NSIZES; s++) {
		size_t size = test_sizes[s];
		for (unsigned i = 0; i < 8; i++) {
			void *ptr = mallocx(size,
			    MALLOCX_ARENA(a) | MALLOCX_TCACHE_NONE);
			expect_ptr_not_null(ptr, "mallocx failed");
			memset(ptr, 0x5a, size);
			dallocx(ptr, MALLOCX_ARENA(b) | MALLOCX_TCACHE_NONE);
		}
	}

	/*
	 * A purge afterwards walks every psset.  An extent that went home to
	 * the wrong shard shows up here as an assertion failure or a crash
	 * rather than as nothing at all.
	 */
	expect_d_eq(mallctl("hpa.purge", NULL, NULL, NULL, 0), 0,
	    "hpa.purge failed");
}
TEST_END

TEST_BEGIN(test_config_took_effect) {
	test_skip_if(!pools_configured());

	/*
	 * The options describe what was asked for; hpa.npools and hpa.nshards
	 * describe what was built.  Check they agree -- the pair exists
	 * precisely because they can disagree silently, when the feature
	 * switch is off or the shard-count clamp fires, leaving a process
	 * running a topology nobody chose.
	 *
	 * Deliberately compared against the built topology rather than
	 * against the literals in this file's malloc_conf: an outer
	 * MALLOC_CONF overrides a test's own settings, and a test that pins
	 * constants it does not control reports the override as a bug.
	 */
	bool     shard_pools;
	size_t   sz = sizeof(shard_pools);
	expect_d_eq(
	    mallctl("opt.hpa_shard_pools", &shard_pools, &sz, NULL, 0), 0,
	    "opt.hpa_shard_pools unreadable");
	expect_true(shard_pools,
	    "pools were built from a configured layout, so the switch must "
	    "read as on");

	const char *pick;
	sz = sizeof(pick);
	expect_d_eq(mallctl("opt.hpa_pool_pick", &pick, &sz, NULL, 0), 0,
	    "opt.hpa_pool_pick unreadable");
	expect_str_eq(pick,
	    hpa_pool_pick_names[hpa_pools_global.pools[0].pick],
	    "opt.hpa_pool_pick disagrees with the picker in use");

	size_t nshards_max;
	sz = sizeof(nshards_max);
	expect_d_eq(
	    mallctl("opt.hpa_pool_nshards_max", &nshards_max, &sz, NULL, 0), 0,
	    "opt.hpa_pool_nshards_max unreadable");

	unsigned npools, nshards;
	sz = sizeof(npools);
	expect_d_eq(mallctl("hpa.npools", &npools, &sz, NULL, 0), 0,
	    "hpa.npools unreadable");
	sz = sizeof(nshards);
	expect_d_eq(mallctl("hpa.nshards", &nshards, &sz, NULL, 0), 0,
	    "hpa.nshards unreadable");

	expect_u_eq(npools, hpa_pools_global.npools,
	    "hpa.npools does not match the pool set");
	expect_u_eq(nshards, hpa_pools_global.nshards_total,
	    "hpa.nshards does not match the pool set");
	expect_zu_le((size_t)nshards, nshards_max,
	    "more shards were built than the cap allows");

	unsigned sum = 0;
	for (unsigned i = 0; i < npools; i++) {
		sum += hpa_pools_global.pools[i].nshards;
	}
	expect_u_eq(sum, nshards,
	    "the pools' shards do not add up to the set's total");
}
TEST_END

int
main(void) {
	return test(test_bands_segregate, test_owner_is_not_the_arena,
	    test_free_from_a_different_arena, test_config_took_effect);
}
