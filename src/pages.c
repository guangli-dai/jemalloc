#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/pages.h"

#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/os.h"

/******************************************************************************/
/* Data. */

/* Actual operating system page size, detected during bootstrap, <= PAGE. */
size_t os_page;

const char *const thp_mode_names[] = {
    "default", "always", "never", "not supported"};
const char *const system_thp_mode_names[] = {
    "madvise", "always", "never", "not supported"};
thp_mode_t        opt_thp = THP_MODE_DEFAULT;
system_thp_mode_t init_system_thp_mode;

/* Runtime support for lazy purge. Irrelevant when !pages_can_purge_lazy. */
static bool pages_can_purge_lazy_runtime = true;

/******************************************************************************/

static void *pages_map_slow(size_t size, size_t alignment, bool *commit);

static void *
pages_trim(
    void *addr, size_t alloc_size, size_t leadsize, size_t size, bool *commit) {
	void *ret = (void *)((byte_t *)addr + leadsize);

	assert(alloc_size >= leadsize + size);
#if OS_VM_CAN_PARTIAL_RELEASE
	size_t trailsize = alloc_size - leadsize - size;

	if (leadsize != 0) {
		os_vm_release(addr, leadsize);
	}
	if (trailsize != 0) {
		os_vm_release((void *)((byte_t *)ret + size), trailsize);
	}
	return ret;
#else
	os_vm_release(addr, alloc_size);
	void *new_addr = os_vm_reserve(ret, size, PAGE, commit);
	if (new_addr == ret) {
		return ret;
	}
	if (new_addr != NULL) {
		os_vm_release(new_addr, size);
	}
	return NULL;
#endif
}

static void *
pages_map_slow(size_t size, size_t alignment, bool *commit) {
	size_t alloc_size = size + alignment - os_page;
	/* Beware size_t wrap-around. */
	if (alloc_size < size) {
		return NULL;
	}

	void *ret;
	do {
		void *pages = os_vm_reserve(NULL, alloc_size, os_page, commit);
		if (pages == NULL) {
			return NULL;
		}
		size_t leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment)
		    - (uintptr_t)pages;
		ret = pages_trim(pages, alloc_size, leadsize, size, commit);
	} while (ret == NULL);

	assert(ret != NULL);
	assert(PAGE_ADDR2BASE(ret) == ret);
	return ret;
}

void *
pages_map(void *addr, size_t size, size_t alignment, bool *commit) {
	assert(alignment >= PAGE);
	assert(ALIGNMENT_ADDR2BASE(addr, alignment) == addr);

	void *ret = os_vm_reserve(addr, size, alignment, commit);
	if (ret == NULL) {
		return NULL;
	}
	/*
	 * If we asked for a specific hint we got it (os_vm_reserve already
	 * rejects mismatches), or if the natively-aligned reserve satisfied
	 * alignment, we're done.
	 */
	if (ret == addr || ALIGNMENT_ADDR2OFFSET(ret, alignment) == 0) {
		assert(addr == NULL || ret == addr);
		assert(PAGE_ADDR2BASE(ret) == ret);
		return ret;
	}
	assert(addr == NULL);
	os_vm_release(ret, size);
	return pages_map_slow(size, alignment, commit);
}

void
pages_unmap(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	os_vm_release(addr, size);
}

static bool
pages_commit_impl(void *addr, size_t size, bool commit) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	if (os_vm_overcommit_mode()) {
		return true;
	}

	return commit ? os_vm_commit(addr, size) : os_vm_decommit(addr, size);
}

bool
pages_commit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, true);
}

bool
pages_decommit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, false);
}

void
pages_mark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
	os_vm_mark_guards(head, tail);
}

void
pages_unmark_guards(void *head, void *tail) {
	assert(head != NULL || tail != NULL);
	assert(
	    head == NULL || tail == NULL || (uintptr_t)head < (uintptr_t)tail);
	os_vm_unmark_guards(head, tail);
}

bool
pages_purge_lazy(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_lazy) {
		return true;
	}
	if (!pages_can_purge_lazy_runtime) {
		/*
		 * Built with lazy purge enabled, but detected it was not
		 * supported on the current system.
		 */
		return true;
	}

	return os_vm_purge_lazy(addr, size);
}

bool
pages_purge_forced(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_forced) {
		return true;
	}
	return os_vm_purge_forced(addr, size);
}

bool
pages_purge_process_madvise(void *vec, size_t vec_len, size_t total_bytes) {
	return os_vm_batch_purge(vec, vec_len, total_bytes);
}

bool
pages_huge(void *addr, size_t size) {
	assert(HUGEPAGE_ADDR2BASE(addr) == addr);
	assert(HUGEPAGE_CEILING(size) == size);
	return os_vm_hugepage(addr, size);
}

bool
pages_nohuge(void *addr, size_t size) {
	assert(HUGEPAGE_ADDR2BASE(addr) == addr);
	assert(HUGEPAGE_CEILING(size) == size);
	return os_vm_nohugepage(addr, size);
}

bool
pages_collapse(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
	/*
	 * MADV_COLLAPSE has one more precondition the assertions above don't
	 * cover: at least one page in the range must currently be backed by
	 * physical memory. See madvise(2). Callers must not invoke this on
	 * freshly mapped regions.
	 */
	return os_vm_huge_collapse(addr, size);
}

bool
pages_dontdump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
	return os_vm_dontdump(addr, size);
}

bool
pages_dodump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
	return os_vm_dodump(addr, size);
}

static bool
pages_should_skip_set_thp_state() {
	if (opt_thp == thp_mode_do_nothing
	    || (opt_thp == thp_mode_always
	        && init_system_thp_mode == system_thp_mode_always)
	    || (opt_thp == thp_mode_never
	        && init_system_thp_mode == system_thp_mode_never)) {
		return true;
	}
	return false;
}

void
pages_set_thp_state(void *ptr, size_t size) {
	if (pages_should_skip_set_thp_state()) {
		return;
	}
	assert(opt_thp != thp_mode_not_supported
	    && init_system_thp_mode != system_thp_mode_not_supported);

	if (opt_thp == thp_mode_always
	    && init_system_thp_mode == system_thp_mode_madvise) {
		os_vm_hugepage(ptr, size);
	} else if (opt_thp == thp_mode_never) {
		assert(init_system_thp_mode == system_thp_mode_madvise
		    || init_system_thp_mode == system_thp_mode_always);
		os_vm_nohugepage(ptr, size);
	}
}

bool
pages_boot(void) {
	os_page = os_vm_page_size();
	if (os_page > PAGE) {
		malloc_write("<jemalloc>: Unsupported system page size\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}

	if (os_vm_boot()) {
		return true;
	}

#if OS_VM_LAZY_PURGE_NEEDS_RUNTIME_CHECK
	/* Detect lazy purge runtime support. */
	if (pages_can_purge_lazy) {
		bool  committed = false;
		void *madv_free_page = os_vm_reserve(
		    NULL, PAGE, PAGE, &committed);
		if (madv_free_page == NULL) {
			return true;
		}
		assert(pages_can_purge_lazy_runtime);
		if (pages_purge_lazy(madv_free_page, PAGE)) {
			pages_can_purge_lazy_runtime = false;
		}
		os_vm_release(madv_free_page, PAGE);
	}
#endif

	return false;
}
