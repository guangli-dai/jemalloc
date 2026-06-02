/*
 * Darwin OS layer - non-inline implementations for every facility declared
 * in include/jemalloc/internal/os/darwin.h.
 *
 * Section order and banners match os/darwin.h.
 */

#define JEMALLOC_OS_DARWIN_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/os.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

#include <sys/mman.h>

/* ====================================================================
 * VM
 * ==================================================================== */

/* Cached overcommit decision, exposed inline via os_vm_overcommit_mode.
 * Darwin does not expose an overcommit knob, so this is always false. */
bool os_overcommits;

/*
 * mmap flags assembled at boot (MAP_PRIVATE | MAP_ANON). File-local because
 * all callers live in this translation unit.
 */
static int mmap_flags;

#ifdef JEMALLOC_HAVE_VM_MAKE_TAG
#  define PAGES_FD_TAG VM_MAKE_TAG(254U)
#else
#  define PAGES_FD_TAG -1
#endif

#define PAGES_PROT_COMMIT   (PROT_READ | PROT_WRITE)
#define PAGES_PROT_DECOMMIT (PROT_NONE)

void *
os_vm_reserve(void *hint, size_t size, size_t align, bool *commit) {
	assert(ALIGNMENT_ADDR2BASE(hint, os_page) == hint);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	assert(size != 0);
	(void)align;
	if (os_overcommits) {
		*commit = true;
	}
	int   prot = *commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
	void *ret = mmap(hint, size, prot, mmap_flags, PAGES_FD_TAG, 0);
	if (ret == MAP_FAILED) {
		return NULL;
	}
	if (hint != NULL && ret != hint) {
		if (munmap(ret, size) == -1) {
			char buf[BUFERROR_BUF];
			buferror(get_errno(), buf, sizeof(buf));
			malloc_printf(
			    "<jemalloc>: Error in munmap(): %s\n", buf);
			if (opt_abort) {
				abort();
			}
		}
		return NULL;
	}
	return ret;
}

void
os_vm_release(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	if (munmap(addr, size) == -1) {
		char buf[BUFERROR_BUF];
		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in munmap(): %s\n", buf);
		if (opt_abort) {
			abort();
		}
	}
}

static bool
os_vm_commit_impl(void *addr, size_t size, bool commit) {
	int   prot = commit ? PAGES_PROT_COMMIT : PAGES_PROT_DECOMMIT;
	void *result = mmap(
	    addr, size, prot, mmap_flags | MAP_FIXED, PAGES_FD_TAG, 0);
	if (result == MAP_FAILED) {
		return true;
	}
	if (result != addr) {
		os_vm_release(result, size);
		return true;
	}
	return false;
}

bool
os_vm_commit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, true);
}

bool
os_vm_decommit(void *addr, size_t size) {
	return os_vm_commit_impl(addr, size, false);
}

void
os_vm_mark_guards(void *head, void *tail) {
#ifdef JEMALLOC_HAVE_MPROTECT
	if (head != NULL) {
		mprotect(head, PAGE, PROT_NONE);
	}
	if (tail != NULL) {
		mprotect(tail, PAGE, PROT_NONE);
	}
#else
	if (head != NULL) {
		(void)os_vm_commit_impl(head, PAGE, false);
	}
	if (tail != NULL) {
		(void)os_vm_commit_impl(tail, PAGE, false);
	}
#endif
}

void
os_vm_unmark_guards(void *head, void *tail) {
#ifdef JEMALLOC_HAVE_MPROTECT
	bool   head_and_tail = (head != NULL) && (tail != NULL);
	size_t range = head_and_tail ? (uintptr_t)tail - (uintptr_t)head + PAGE
	                             : SIZE_T_MAX;
	bool ranged_mprotect = head_and_tail && range <= SC_LARGE_MINCLASS;
	if (ranged_mprotect) {
		mprotect(head, range, PROT_READ | PROT_WRITE);
	} else {
		if (head != NULL) {
			mprotect(head, PAGE, PROT_READ | PROT_WRITE);
		}
		if (tail != NULL) {
			mprotect(tail, PAGE, PROT_READ | PROT_WRITE);
		}
	}
#else
	if (head != NULL) {
		(void)os_vm_commit_impl(head, PAGE, true);
	}
	if (tail != NULL) {
		(void)os_vm_commit_impl(tail, PAGE, true);
	}
#endif
}

bool
os_vm_purge_lazy(void *addr, size_t size) {
#if defined(JEMALLOC_PURGE_MADVISE_FREE)
	return (madvise(addr, size,
#  ifdef MADV_FREE
	            MADV_FREE
#  else
	            JEMALLOC_MADV_FREE
#  endif
	            )
	    != 0);
#elif defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                 \
    && !defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED)                           \
    && !defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#else
	(void)addr;
	(void)size;
	not_reached();
#endif
}

bool
os_vm_purge_forced(void *addr, size_t size) {
#if defined(JEMALLOC_MAPS_COALESCE)
	return os_vm_commit(addr, size);
#else
	(void)addr;
	(void)size;
	not_reached();
#endif
}

bool
os_vm_purge_forced_is_enabled(void) {
	return true;
}

bool
os_vm_batch_purge(void *vec, size_t vec_len, size_t total_bytes) {
	(void)vec;
	(void)vec_len;
	(void)total_bytes;
	not_reached();
	return true;
}

bool
os_vm_hugepage(void *addr, size_t size) {
	(void)addr;
	(void)size;
	return true;
}

bool
os_vm_nohugepage(void *addr, size_t size) {
	(void)addr;
	(void)size;
	return false;
}

bool
os_vm_huge_collapse(void *addr, size_t size) {
	(void)addr;
	(void)size;
	return true;
}

bool
os_vm_dontdump(void *addr, size_t size) {
	(void)addr;
	(void)size;
	return false;
}

bool
os_vm_dodump(void *addr, size_t size) {
	(void)addr;
	(void)size;
	return false;
}

bool
os_vm_set_name(void *addr, size_t size, const char *name) {
	(void)addr;
	(void)size;
	(void)name;
	return false;
}

size_t
os_vm_page_size(void) {
	long result = sysconf(_SC_PAGESIZE);
	if (result == -1) {
		return PAGE;
	}
	return (size_t)result;
}

int
os_vm_thp_system_mode(void) {
	return (int)init_system_thp_mode;
}

bool
os_vm_boot(void) {
	mmap_flags = MAP_PRIVATE | MAP_ANON;
	os_overcommits = false;
	opt_thp = thp_mode_not_supported;
	init_system_thp_mode = system_thp_mode_not_supported;
	return false;
}

/* ====================================================================
 * Sync (mutex)
 * ==================================================================== */

/* ====================================================================
 * Cond + sigmask
 * ==================================================================== */

/* ====================================================================
 * Time
 * ==================================================================== */

/* ====================================================================
 * Thread
 * ==================================================================== */

/* ====================================================================
 * CPU
 * ==================================================================== */

/* ====================================================================
 * Process
 * ==================================================================== */

bool
os_process_register_atfork(void (*prepare)(void), void (*parent)(void),
    void (*child)(void)) {
	/* Darwin's malloc-zone fork callbacks subsume pthread_atfork. */
	(void)prepare;
	(void)parent;
	(void)child;
	return false;
}

/* ====================================================================
 * File I/O
 * ==================================================================== */

/* ====================================================================
 * DSS / sbrk
 * ==================================================================== */

void *
os_dss_sbrk(intptr_t increment) {
#ifdef JEMALLOC_DSS
	return sbrk(increment);
#else
	(void)increment;
	not_reached();
	return NULL;
#endif
}

/* ====================================================================
 * Darwin malloc zone   (folded in from src/zone.c)
 *
 * Verbatim move of the former src/zone.c, guarded by JEMALLOC_ZONE so
 * non-Darwin builds (which already exclude darwin.c at the Makefile
 * level) and Darwin builds that opted out of the zone allocator
 * compile to nothing.
 * ==================================================================== */
#ifdef JEMALLOC_ZONE

#include "jemalloc/internal/jemalloc_fork.h"

/* Definitions of the following structs in malloc/malloc.h might be too old
 * for the built binary to run on newer versions of OSX. So use the newest
 * possible version of those structs.
 */
typedef struct _malloc_zone_t {
	void *reserved1;
	void *reserved2;
	size_t (*size)(struct _malloc_zone_t *, const void *);
	void *(*malloc)(struct _malloc_zone_t *, size_t);
	void *(*calloc)(struct _malloc_zone_t *, size_t, size_t);
	void *(*valloc)(struct _malloc_zone_t *, size_t);
	void (*free)(struct _malloc_zone_t *, void *);
	void *(*realloc)(struct _malloc_zone_t *, void *, size_t);
	void (*destroy)(struct _malloc_zone_t *);
	const char *zone_name;
	unsigned (*batch_malloc)(
	    struct _malloc_zone_t *, size_t, void **, unsigned);
	void (*batch_free)(struct _malloc_zone_t *, void **, unsigned);
	struct malloc_introspection_t *introspect;
	unsigned                       version;
	void *(*memalign)(struct _malloc_zone_t *, size_t, size_t);
	void (*free_definite_size)(struct _malloc_zone_t *, void *, size_t);
	size_t (*pressure_relief)(struct _malloc_zone_t *, size_t);
} malloc_zone_t;

typedef struct {
	vm_address_t address;
	vm_size_t    size;
} vm_range_t;

typedef struct malloc_statistics_t {
	unsigned blocks_in_use;
	size_t   size_in_use;
	size_t   max_size_in_use;
	size_t   size_allocated;
} malloc_statistics_t;

typedef kern_return_t memory_reader_t(task_t, vm_address_t, vm_size_t, void **);

typedef void vm_range_recorder_t(
    task_t, void *, unsigned type, vm_range_t *, unsigned);

typedef struct malloc_introspection_t {
	kern_return_t (*enumerator)(task_t, void *, unsigned, vm_address_t,
	    memory_reader_t, vm_range_recorder_t);
	size_t (*good_size)(malloc_zone_t *, size_t);
	boolean_t (*check)(malloc_zone_t *);
	void (*print)(malloc_zone_t *, boolean_t);
	void (*log)(malloc_zone_t *, void *);
	void (*force_lock)(malloc_zone_t *);
	void (*force_unlock)(malloc_zone_t *);
	void (*statistics)(malloc_zone_t *, malloc_statistics_t *);
	boolean_t (*zone_locked)(malloc_zone_t *);
	boolean_t (*enable_discharge_checking)(malloc_zone_t *);
	boolean_t (*disable_discharge_checking)(malloc_zone_t *);
	void (*discharge)(malloc_zone_t *, void *);
#ifdef __BLOCKS__
	void (*enumerate_discharged_pointers)(
	    malloc_zone_t *, void (^)(void *, void *));
#else
	void *enumerate_unavailable_without_blocks;
#endif
	void (*reinit_lock)(malloc_zone_t *);
} malloc_introspection_t;

extern kern_return_t malloc_get_all_zones(
    task_t, memory_reader_t, vm_address_t **, unsigned *);

extern malloc_zone_t *malloc_default_zone(void);

extern void malloc_zone_register(malloc_zone_t *zone);

extern void malloc_zone_unregister(malloc_zone_t *zone);

/*
 * The malloc_default_purgeable_zone() function is only available on >= 10.6.
 * We need to check whether it is present at runtime, thus the weak_import.
 */
extern malloc_zone_t *malloc_default_purgeable_zone(void)
    JEMALLOC_ATTR(weak_import);

/******************************************************************************/
/* Data. */

static malloc_zone_t                *default_zone, *purgeable_zone;
static malloc_zone_t                 jemalloc_zone;
static struct malloc_introspection_t jemalloc_zone_introspect;
static pid_t                         zone_force_lock_pid = -1;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static size_t zone_size(malloc_zone_t *zone, const void *ptr);
static void  *zone_malloc(malloc_zone_t *zone, size_t size);
static void  *zone_calloc(malloc_zone_t *zone, size_t num, size_t size);
static void  *zone_valloc(malloc_zone_t *zone, size_t size);
static void   zone_free(malloc_zone_t *zone, void *ptr);
static void  *zone_realloc(malloc_zone_t *zone, void *ptr, size_t size);
static void  *zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size);
static void   zone_free_definite_size(
      malloc_zone_t *zone, void *ptr, size_t size);
static void     zone_destroy(malloc_zone_t *zone);
static unsigned zone_batch_malloc(struct _malloc_zone_t *zone, size_t size,
    void **results, unsigned num_requested);
static void     zone_batch_free(
        struct _malloc_zone_t *zone, void **to_be_freed, unsigned num_to_be_freed);
static size_t zone_pressure_relief(struct _malloc_zone_t *zone, size_t goal);
static size_t zone_good_size(malloc_zone_t *zone, size_t size);
static kern_return_t zone_enumerator(task_t task, void *data,
    unsigned type_mask, vm_address_t zone_address, memory_reader_t reader,
    vm_range_recorder_t recorder);
static boolean_t     zone_check(malloc_zone_t *zone);
static void          zone_print(malloc_zone_t *zone, boolean_t verbose);
static void          zone_log(malloc_zone_t *zone, void *address);
static void          zone_force_lock(malloc_zone_t *zone);
static void          zone_force_unlock(malloc_zone_t *zone);
static void zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats);
static boolean_t zone_locked(malloc_zone_t *zone);
static void      zone_reinit_lock(malloc_zone_t *zone);

/******************************************************************************/
/*
 * Functions.
 */

static size_t
zone_size(malloc_zone_t *zone, const void *ptr) {
	/*
	 * There appear to be places within Darwin (such as setenv(3)) that
	 * cause calls to this function with pointers that *no* zone owns.  If
	 * we knew that all pointers were owned by *some* zone, we could split
	 * our zone into two parts, and use one as the default allocator and
	 * the other as the default deallocator/reallocator.  Since that will
	 * not work in practice, we must check all pointers to assure that they
	 * reside within a mapped extent before determining size.
	 */
	return ivsalloc(tsdn_fetch(), ptr);
}

static void *
zone_malloc(malloc_zone_t *zone, size_t size) {
	return je_malloc(size);
}

static void *
zone_calloc(malloc_zone_t *zone, size_t num, size_t size) {
	return je_calloc(num, size);
}

static void *
zone_valloc(malloc_zone_t *zone, size_t size) {
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	je_posix_memalign(&ret, PAGE, size);

	return ret;
}

static void
zone_free(malloc_zone_t *zone, void *ptr) {
	if (ivsalloc(tsdn_fetch(), ptr) != 0) {
		je_free(ptr);
		return;
	}

	free(ptr);
}

static void *
zone_realloc(malloc_zone_t *zone, void *ptr, size_t size) {
	if (ivsalloc(tsdn_fetch(), ptr) != 0) {
		return je_realloc(ptr, size);
	}

	return realloc(ptr, size);
}

static void *
zone_memalign(malloc_zone_t *zone, size_t alignment, size_t size) {
	void *ret = NULL; /* Assignment avoids useless compiler warning. */

	je_posix_memalign(&ret, alignment, size);

	return ret;
}

static void
zone_free_definite_size(malloc_zone_t *zone, void *ptr, size_t size) {
	size_t alloc_size;

	alloc_size = ivsalloc(tsdn_fetch(), ptr);
	if (alloc_size != 0) {
		assert(alloc_size == size);
		je_free(ptr);
		return;
	}

	free(ptr);
}

static void
zone_destroy(malloc_zone_t *zone) {
	/* This function should never be called. */
	not_reached();
}

static unsigned
zone_batch_malloc(struct _malloc_zone_t *zone, size_t size, void **results,
    unsigned num_requested) {
	unsigned i;

	for (i = 0; i < num_requested; i++) {
		results[i] = je_malloc(size);
		if (!results[i])
			break;
	}

	return i;
}

static void
zone_batch_free(
    struct _malloc_zone_t *zone, void **to_be_freed, unsigned num_to_be_freed) {
	unsigned i;

	for (i = 0; i < num_to_be_freed; i++) {
		zone_free(zone, to_be_freed[i]);
		to_be_freed[i] = NULL;
	}
}

static size_t
zone_pressure_relief(struct _malloc_zone_t *zone, size_t goal) {
	return 0;
}

static size_t
zone_good_size(malloc_zone_t *zone, size_t size) {
	if (size == 0) {
		size = 1;
	}
	return sz_s2u(size);
}

static kern_return_t
zone_enumerator(task_t task, void *data, unsigned type_mask,
    vm_address_t zone_address, memory_reader_t reader,
    vm_range_recorder_t recorder) {
	return KERN_SUCCESS;
}

static boolean_t
zone_check(malloc_zone_t *zone) {
	return true;
}

static void
zone_print(malloc_zone_t *zone, boolean_t verbose) {}

static void
zone_log(malloc_zone_t *zone, void *address) {}

static void
zone_force_lock(malloc_zone_t *zone) {
	if (isthreaded) {
		/*
		 * See the note in zone_force_unlock, below, to see why we need
		 * this.
		 */
		assert(zone_force_lock_pid == -1);
		zone_force_lock_pid = getpid();
		jemalloc_prefork();
	}
}

static void
zone_force_unlock(malloc_zone_t *zone) {
	/*
	 * zone_force_lock and zone_force_unlock are the entry points to the
	 * forking machinery on OS X.  The tricky thing is, the child is not
	 * allowed to unlock mutexes locked in the parent, even if owned by the
	 * forking thread (and the mutex type we use in OS X will fail an assert
	 * if we try).  In the child, we can get away with reinitializing all
	 * the mutexes, which has the effect of unlocking them.  In the parent,
	 * doing this would mean we wouldn't wake any waiters blocked on the
	 * mutexes we unlock.  So, we record the pid of the current thread in
	 * zone_force_lock, and use that to detect if we're in the parent or
	 * child here, to decide which unlock logic we need.
	 */
	if (isthreaded) {
		assert(zone_force_lock_pid != -1);
		if (getpid() == zone_force_lock_pid) {
			jemalloc_postfork_parent();
		} else {
			jemalloc_postfork_child();
		}
		zone_force_lock_pid = -1;
	}
}

static void
zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats) {
	/* We make no effort to actually fill the values */
	stats->blocks_in_use = 0;
	stats->size_in_use = 0;
	stats->max_size_in_use = 0;
	stats->size_allocated = 0;
}

static boolean_t
zone_locked(malloc_zone_t *zone) {
	/* Pretend no lock is being held */
	return false;
}

static void
zone_reinit_lock(malloc_zone_t *zone) {
	/* As of OSX 10.12, this function is only used when force_unlock would
	 * be used if the zone version were < 9. So just use force_unlock. */
	zone_force_unlock(zone);
}

static void
zone_init(void) {
	jemalloc_zone.size = zone_size;
	jemalloc_zone.malloc = zone_malloc;
	jemalloc_zone.calloc = zone_calloc;
	jemalloc_zone.valloc = zone_valloc;
	jemalloc_zone.free = zone_free;
	jemalloc_zone.realloc = zone_realloc;
	jemalloc_zone.destroy = zone_destroy;
	jemalloc_zone.zone_name = "jemalloc_zone";
	jemalloc_zone.batch_malloc = zone_batch_malloc;
	jemalloc_zone.batch_free = zone_batch_free;
	jemalloc_zone.introspect = &jemalloc_zone_introspect;
	jemalloc_zone.version = 9;
	jemalloc_zone.memalign = zone_memalign;
	jemalloc_zone.free_definite_size = zone_free_definite_size;
	jemalloc_zone.pressure_relief = zone_pressure_relief;

	jemalloc_zone_introspect.enumerator = zone_enumerator;
	jemalloc_zone_introspect.good_size = zone_good_size;
	jemalloc_zone_introspect.check = zone_check;
	jemalloc_zone_introspect.print = zone_print;
	jemalloc_zone_introspect.log = zone_log;
	jemalloc_zone_introspect.force_lock = zone_force_lock;
	jemalloc_zone_introspect.force_unlock = zone_force_unlock;
	jemalloc_zone_introspect.statistics = zone_statistics;
	jemalloc_zone_introspect.zone_locked = zone_locked;
	jemalloc_zone_introspect.enable_discharge_checking = NULL;
	jemalloc_zone_introspect.disable_discharge_checking = NULL;
	jemalloc_zone_introspect.discharge = NULL;
#ifdef __BLOCKS__
	jemalloc_zone_introspect.enumerate_discharged_pointers = NULL;
#else
	jemalloc_zone_introspect.enumerate_unavailable_without_blocks = NULL;
#endif
	jemalloc_zone_introspect.reinit_lock = zone_reinit_lock;
}

static malloc_zone_t *
zone_default_get(void) {
	malloc_zone_t **zones = NULL;
	unsigned int    num_zones = 0;

	/*
	 * On OSX 10.12, malloc_default_zone returns a special zone that is not
	 * present in the list of registered zones. That zone uses a "lite zone"
	 * if one is present (apparently enabled when malloc stack logging is
	 * enabled), or the first registered zone otherwise. In practice this
	 * means unless malloc stack logging is enabled, the first registered
	 * zone is the default.  So get the list of zones to get the first one,
	 * instead of relying on malloc_default_zone.
	 */
	if (KERN_SUCCESS
	    != malloc_get_all_zones(
	        0, NULL, (vm_address_t **)&zones, &num_zones)) {
		/*
		 * Reset the value in case the failure happened after it was
		 * set.
		 */
		num_zones = 0;
	}

	if (num_zones) {
		return zones[0];
	}

	return malloc_default_zone();
}

/* As written, this function can only promote jemalloc_zone. */
static void
zone_promote(void) {
	malloc_zone_t *zone;

	do {
		/*
		 * Unregister and reregister the default zone.  On OSX >= 10.6,
		 * unregistering takes the last registered zone and places it
		 * at the location of the specified zone.  Unregistering the
		 * default zone thus makes the last registered one the default.
		 * On OSX < 10.6, unregistering shifts all registered zones.
		 * The first registered zone then becomes the default.
		 */
		malloc_zone_unregister(default_zone);
		malloc_zone_register(default_zone);

		/*
		 * On OSX 10.6, having the default purgeable zone appear before
		 * the default zone makes some things crash because it thinks it
		 * owns the default zone allocated pointers.  We thus
		 * unregister/re-register it in order to ensure it's always
		 * after the default zone.  On OSX < 10.6, there is no purgeable
		 * zone, so this does nothing.  On OSX >= 10.6, unregistering
		 * replaces the purgeable zone with the last registered zone
		 * above, i.e. the default zone.  Registering it again then puts
		 * it at the end, obviously after the default zone.
		 */
		if (purgeable_zone != NULL) {
			malloc_zone_unregister(purgeable_zone);
			malloc_zone_register(purgeable_zone);
		}

		zone = zone_default_get();
	} while (zone != &jemalloc_zone);
}

JEMALLOC_ATTR(constructor)
void
zone_register(void) {
	/*
	 * If something else replaced the system default zone allocator, don't
	 * register jemalloc's.
	 */
	default_zone = zone_default_get();
	if (!default_zone->zone_name
	    || strcmp(default_zone->zone_name, "DefaultMallocZone") != 0) {
		return;
	}

	/*
	 * The default purgeable zone is created lazily by OSX's libc.  It uses
	 * the default zone when it is created for "small" allocations
	 * (< 15 KiB), but assumes the default zone is a scalable_zone.  This
	 * obviously fails when the default zone is the jemalloc zone, so
	 * malloc_default_purgeable_zone() is called beforehand so that the
	 * default purgeable zone is created when the default zone is still
	 * a scalable_zone.  As purgeable zones only exist on >= 10.6, we need
	 * to check for the existence of malloc_default_purgeable_zone() at
	 * run time.
	 */
	purgeable_zone = (malloc_default_purgeable_zone == NULL)
	    ? NULL
	    : malloc_default_purgeable_zone();

	/* Register the custom zone.  At this point it won't be the default. */
	zone_init();
	malloc_zone_register(&jemalloc_zone);

	/* Promote the custom zone to be default. */
	zone_promote();
}

#endif /* JEMALLOC_ZONE */
