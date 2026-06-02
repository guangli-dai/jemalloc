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
