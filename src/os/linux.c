/*
 * Linux OS layer - non-inline implementations for every facility declared
 * in include/jemalloc/internal/os/linux.h.
 *
 * Section order and banners match os/linux.h.
 */

#define JEMALLOC_OS_LINUX_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/os.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

#include <sys/mman.h>

#ifdef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
#  include <fcntl.h>
#endif

#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
#  include <sys/syscall.h>
#  ifndef PIDFD_SELF
#    define PIDFD_SELF -10000
#  endif
#  ifdef SYS_process_madvise
#    define JE_SYS_PROCESS_MADVISE_NR SYS_process_madvise
#  else
#    define JE_SYS_PROCESS_MADVISE_NR EXPERIMENTAL_SYS_PROCESS_MADVISE_NR
#  endif
#endif

#if defined(JEMALLOC_HAVE_PRCTL) && defined(JEMALLOC_PAGEID)
#  include <sys/prctl.h>
#  ifndef PR_SET_VMA
#    define PR_SET_VMA 0x53564d41
#    define PR_SET_VMA_ANON_NAME 0
#  endif
#endif

/* ====================================================================
 * VM
 * ==================================================================== */

/* Cached overcommit decision, exposed inline via os_vm_overcommit_mode. */
bool os_overcommits;

/*
 * mmap flags assembled at boot (MAP_PRIVATE | MAP_ANON, plus MAP_NORESERVE
 * when the kernel overcommits). File-local because all callers live in this
 * translation unit.
 */
static int    mmap_flags;

#ifdef JEMALLOC_HAVE_VM_MAKE_TAG
#  define PAGES_FD_TAG VM_MAKE_TAG(254U)
#else
#  define PAGES_FD_TAG -1
#endif

#define PAGES_PROT_COMMIT   (PROT_READ | PROT_WRITE)
#define PAGES_PROT_DECOMMIT (PROT_NONE)

#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
/* -1 = not yet probed, 0 = MADV_DONTNEED works, 1 = faulty (QEMU). */
static int madvise_dont_need_zeros_is_faulty = -1;

/*
 * Check that MADV_DONTNEED will actually zero pages on subsequent access.
 * QEMU does not honor this; running jemalloc under QEMU without this gate
 * trips assertions in extent.c. See the original pages.c comment for the
 * upstream QEMU patchwork link.
 */
static int
madvise_MADV_DONTNEED_zeroes_pages(void) {
	size_t size = PAGE;

	void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (addr == MAP_FAILED) {
		malloc_write(
		    "<jemalloc>: Cannot allocate memory for "
		    "MADV_DONTNEED check\n");
		if (opt_abort) {
			abort();
		}
	}

	memset(addr, 'A', size);
	int works;
	if (madvise(addr, size, MADV_DONTNEED) == 0) {
		works = memchr(addr, 'A', size) == NULL;
	} else {
		works = 1;
	}

	if (munmap(addr, size) != 0) {
		malloc_write(
		    "<jemalloc>: Cannot deallocate memory for "
		    "MADV_DONTNEED check\n");
		if (opt_abort) {
			abort();
		}
	}

	return works;
}
#endif

#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
static atomic_b_t process_madvise_gate = ATOMIC_INIT(true);
#endif

#ifdef JEMALLOC_PAGEID
static void
os_vm_set_name_prctl(void *addr, size_t size, const char *name) {
	int n;
	assert(addr != NULL);
	n = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (uintptr_t)addr, size,
	    (uintptr_t)name);
	assert(n == 0 || (n == -1 && get_errno() == EINVAL));
	(void)n;
}
#endif

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
		/* Got a mapping at a different address than requested. */
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
#ifdef JEMALLOC_PAGEID
	os_vm_set_name_prctl(ret, size,
	    os_overcommits ? "jemalloc_pg_overcommit" : "jemalloc_pg");
#endif
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
	/*
	 * mprotect's cost on Linux is largely independent of range up to a
	 * threshold; one ranged call is cheaper than two if the gap is small.
	 */
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
#elif defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                \
    && !defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED)                          \
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
#if defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                  \
    && defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (unlikely(madvise_dont_need_zeros_is_faulty)
	    || madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED)                          \
    && defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (unlikely(madvise_dont_need_zeros_is_faulty)
	    || posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_MAPS_COALESCE)
	return os_vm_commit(addr, size);
#else
	(void)addr;
	(void)size;
	not_reached();
#endif
}

bool
os_vm_purge_forced_is_enabled(void) {
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
	return !madvise_dont_need_zeros_is_faulty;
#else
	return true;
#endif
}

bool
os_vm_batch_purge(void *vec, size_t vec_len, size_t total_bytes) {
#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
	if (!atomic_load_b(&process_madvise_gate, ATOMIC_RELAXED)) {
		return true;
	}

	int    saved_errno = get_errno();
	size_t purged_bytes = (size_t)syscall(JE_SYS_PROCESS_MADVISE_NR,
	    PIDFD_SELF, (struct iovec *)vec, vec_len, MADV_DONTNEED, 0);
	if (purged_bytes == (size_t)-1) {
		if (errno == EPERM || errno == EINVAL || errno == ENOSYS
		    || errno == EBADF) {
			atomic_store_b(
			    &process_madvise_gate, false, ATOMIC_RELAXED);
		}
		set_errno(saved_errno);
	}
	return purged_bytes != total_bytes;
#else
	(void)vec;
	(void)vec_len;
	(void)total_bytes;
	not_reached();
	return true;
#endif
}

bool
os_vm_hugepage(void *addr, size_t size) {
#ifdef JEMALLOC_HAVE_MADVISE_HUGE
	return (madvise(addr, size, MADV_HUGEPAGE) != 0);
#else
	(void)addr;
	(void)size;
	return true;
#endif
}

bool
os_vm_nohugepage(void *addr, size_t size) {
#ifdef JEMALLOC_HAVE_MADVISE_HUGE
	return (madvise(addr, size, MADV_NOHUGEPAGE) != 0);
#else
	(void)addr;
	(void)size;
	return false;
#endif
}

bool
os_vm_huge_collapse(void *addr, size_t size) {
#if defined(JEMALLOC_HAVE_MADVISE_COLLAPSE)                                   \
    && (defined(MADV_COLLAPSE) || defined(JEMALLOC_MADV_COLLAPSE))
#  if defined(MADV_COLLAPSE)
	return (madvise(addr, size, MADV_COLLAPSE) != 0);
#  else
	return (madvise(addr, size, JEMALLOC_MADV_COLLAPSE) != 0);
#  endif
#else
	(void)addr;
	(void)size;
	return true;
#endif
}

bool
os_vm_dontdump(void *addr, size_t size) {
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DONTDUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_NOCORE) != 0;
#else
	(void)addr;
	(void)size;
	return false;
#endif
}

bool
os_vm_dodump(void *addr, size_t size) {
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DODUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_CORE) != 0;
#else
	(void)addr;
	(void)size;
	return false;
#endif
}

bool
os_vm_set_name(void *addr, size_t size, const char *name) {
#if defined(JEMALLOC_HAVE_PRCTL) && defined(JEMALLOC_PAGEID)
	os_vm_set_name_prctl(addr, size, name);
	return false;
#else
	(void)addr;
	(void)size;
	(void)name;
	return false;
#endif
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

#ifdef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
static bool
os_overcommits_proc(void) {
	int  fd;
	char buf[1];

#  if defined(O_CLOEXEC)
	fd = os_file_open(
	    "/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC);
#  else
	fd = os_file_open("/proc/sys/vm/overcommit_memory", O_RDONLY);
	if (fd != -1) {
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
#  endif

	if (fd == -1) {
		return false;
	}

	ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
	os_file_close(fd);

	if (nread < 1) {
		return false;
	}
	/*
	 * /proc/sys/vm/overcommit_memory meanings:
	 *   0: heuristic overcommit, 1: always overcommit, 2: never overcommit.
	 */
	return (buf[0] == '0' || buf[0] == '1');
}
#endif

static void
init_thp_state(void) {
	if (!have_madvise_huge && !have_memcntl) {
		if (metadata_thp_enabled() && opt_abort) {
			malloc_write("<jemalloc>: no MADV_HUGEPAGE support\n");
			abort();
		}
		goto label_error;
	}
#if defined(JEMALLOC_HAVE_MADVISE_HUGE)
	static const char sys_state_madvise[] = "always [madvise] never\n";
	static const char sys_state_always[] = "[always] madvise never\n";
	static const char sys_state_never[] = "always madvise [never]\n";
	char              buf[sizeof(sys_state_madvise)];

	int fd = os_file_open(
	    "/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
	if (fd == -1) {
		goto label_error;
	}

	ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
	os_file_close(fd);
	if (nread < 0) {
		goto label_error;
	}

	if (strncmp(buf, sys_state_madvise, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_madvise;
	} else if (strncmp(buf, sys_state_always, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_always;
	} else if (strncmp(buf, sys_state_never, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_never;
	} else {
		goto label_error;
	}
	if (opt_hpa_opts.hugify_style == hpa_hugify_style_auto) {
		if (init_system_thp_mode == system_thp_mode_madvise) {
			opt_hpa_opts.hugify_style = hpa_hugify_style_lazy;
		} else {
			opt_hpa_opts.hugify_style = hpa_hugify_style_none;
		}
	}
	return;
#elif defined(JEMALLOC_HAVE_MEMCNTL)
	init_system_thp_mode = system_thp_mode_madvise;
	if (opt_hpa_opts.hugify_style == hpa_hugify_style_auto) {
		opt_hpa_opts.hugify_style = hpa_hugify_style_eager;
	}
	return;
#endif
label_error:
	opt_thp = thp_mode_not_supported;
	init_system_thp_mode = system_thp_mode_not_supported;
}

#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
static bool
init_process_madvise(void) {
	if (opt_process_madvise_max_batch == 0) {
		return false;
	}
	if (opt_process_madvise_max_batch > PROCESS_MADVISE_MAX_BATCH_LIMIT) {
		opt_process_madvise_max_batch = PROCESS_MADVISE_MAX_BATCH_LIMIT;
	}
	return false;
}
#else
static bool
init_process_madvise(void) {
	return false;
}
#endif

bool
os_vm_boot(void) {
#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
	if (!opt_trust_madvise) {
		madvise_dont_need_zeros_is_faulty =
		    !madvise_MADV_DONTNEED_zeroes_pages();
		if (madvise_dont_need_zeros_is_faulty) {
			malloc_write(
			    "<jemalloc>: MADV_DONTNEED does not work (memset will be used instead)\n");
			malloc_write(
			    "<jemalloc>: (This is the expected behaviour if you are running under QEMU)\n");
		}
	} else {
		madvise_dont_need_zeros_is_faulty = 0;
	}
#endif

	mmap_flags = MAP_PRIVATE | MAP_ANON;

#ifdef JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY
	os_overcommits = os_overcommits_proc();
#  ifdef MAP_NORESERVE
	if (os_overcommits) {
		mmap_flags |= MAP_NORESERVE;
	}
#  endif
#else
	os_overcommits = false;
#endif

	init_thp_state();

	if (init_process_madvise()) {
		return true;
	}
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
#ifdef JEMALLOC_HAVE_PTHREAD_ATFORK
	/* LinuxThreads' pthread_atfork() allocates. */
	return pthread_atfork(prepare, parent, child) != 0;
#else
	(void)prepare;
	(void)parent;
	(void)child;
	return false;
#endif
}

/* ====================================================================
 * File I/O
 * ==================================================================== */

/* ====================================================================
 * /proc and /sys reads
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
