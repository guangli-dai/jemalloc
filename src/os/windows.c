/*
 * Windows OS layer - non-inline implementations for every facility
 * declared in include/jemalloc/internal/os/windows.h.
 */

#define JEMALLOC_OS_WINDOWS_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/os.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

/* ====================================================================
 * VM
 * ==================================================================== */

/*
 * Windows always treats reservations as best-effort: VirtualAlloc with
 * MEM_RESERVE does not commit physical backing until MEM_COMMIT, so
 * "overcommit" is structurally always-on. The os_overcommits global is
 * still defined so the os_vm_overcommit_mode contract stays uniform.
 */
bool os_overcommits;

void *
os_vm_reserve(void *hint, size_t size, size_t align, bool *commit) {
	assert(ALIGNMENT_ADDR2BASE(hint, os_page) == hint);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	assert(size != 0);
	(void)align;
	void *ret = VirtualAlloc(hint, size,
	    MEM_RESERVE | (*commit ? MEM_COMMIT : 0), PAGE_READWRITE);
	return ret;
}

void
os_vm_release(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(ALIGNMENT_CEILING(size, os_page) == size);
	(void)size;
	if (VirtualFree(addr, 0, MEM_RELEASE) == 0) {
		char buf[BUFERROR_BUF];
		buferror(get_errno(), buf, sizeof(buf));
		malloc_printf("<jemalloc>: Error in VirtualFree(): %s\n", buf);
		if (opt_abort) {
			abort();
		}
	}
}

bool
os_vm_commit(void *addr, size_t size) {
	return addr != VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
}

bool
os_vm_decommit(void *addr, size_t size) {
	return !VirtualFree(addr, size, MEM_DECOMMIT);
}

void
os_vm_mark_guards(void *head, void *tail) {
	/* Decommit sets the pages to no-access on Windows. */
	if (head != NULL) {
		(void)os_vm_decommit(head, PAGE);
	}
	if (tail != NULL) {
		(void)os_vm_decommit(tail, PAGE);
	}
}

void
os_vm_unmark_guards(void *head, void *tail) {
	if (head != NULL) {
		(void)os_vm_commit(head, PAGE);
	}
	if (tail != NULL) {
		(void)os_vm_commit(tail, PAGE);
	}
}

bool
os_vm_purge_lazy(void *addr, size_t size) {
	VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
	return false;
}

bool
os_vm_purge_forced(void *addr, size_t size) {
	(void)addr;
	(void)size;
	/* Forced purge is intentionally off on Windows (see pages.h note). */
	not_reached();
}

bool
os_vm_purge_forced_is_enabled(void) {
	return false;
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
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (size_t)si.dwPageSize;
}

int
os_vm_thp_system_mode(void) {
	return (int)init_system_thp_mode;
}

bool
os_vm_boot(void) {
	os_overcommits = false;
	opt_thp = thp_mode_not_supported;
	init_system_thp_mode = system_thp_mode_not_supported;
	return false;
}

/* ====================================================================
 * Sync (mutex)
 * ==================================================================== */

/* ====================================================================
 * Cond   (no sigmask on Windows)
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
	/* Windows has no fork(2)/pthread_atfork. */
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
 *
 * Windows has no sbrk(2); extent_dss.c never reaches this on a
 * Windows build (OS_DSS_HAS_SBRK = 0 routes around it), so the body is
 * unreachable.
 * ==================================================================== */

void *
os_dss_sbrk(intptr_t increment) {
	(void)increment;
	not_reached();
	return NULL;
}

/* ====================================================================
 * Windows DLL TLS callback (.CRT$XLY)   (folded in from tsd.c)
 *
 * The linker scans every object file for a .CRT$XLY section and emits a
 * TLS-callback table from the union, so this binding works equally well
 * here as it did in tsd.c.
 * ==================================================================== */

static BOOL WINAPI
_tls_callback(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	switch (fdwReason) {
#ifdef JEMALLOC_LAZY_LOCK
	case DLL_THREAD_ATTACH:
		isthreaded = true;
		break;
#endif
	case DLL_THREAD_DETACH:
		_malloc_thread_cleanup();
		break;
	default:
		break;
	}
	return true;
}

/*
 * We need to be able to say "read" here (in the "pragma section"), but
 * have hooked "read". We won't read for the rest of the file, so we can
 * get away with unhooking.
 */
#ifdef read
#  undef read
#endif

#ifdef _MSC_VER
#  ifdef _M_IX86
#    pragma comment(linker, "/INCLUDE:__tls_used")
#    pragma comment(linker, "/INCLUDE:_tls_callback")
#  else
#    pragma comment(linker, "/INCLUDE:_tls_used")
#    pragma comment(linker, "/INCLUDE:" STRINGIFY(tls_callback))
#  endif
#  pragma section(".CRT$XLY", long, read)
#endif
JEMALLOC_SECTION(".CRT$XLY")
JEMALLOC_ATTR(used) BOOL(WINAPI *const tls_callback)(
    HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) = _tls_callback;

/* ====================================================================
 * Windows static-ctor init lock (.CRT$XCU)
 *   (folded in from jemalloc_init.c)
 *
 * Vista+ uses SRWLOCK_INIT for a compile-time initializer; pre-Vista
 * needs a runtime constructor scheduled via the .CRT$XCU section table
 * (and called explicitly from malloc_init_hard as a belt-and-suspenders
 * guard against constructor ordering races).
 *
 * init_lock has external linkage so jemalloc_init.c can lock/unlock it;
 * see the extern declaration in include/jemalloc/internal/os/windows.h.
 * ==================================================================== */

#if _WIN32_WINNT >= 0x0600
malloc_mutex_t init_lock = SRWLOCK_INIT;
#else
malloc_mutex_t init_lock;
static bool    init_lock_initialized = false;

JEMALLOC_ATTR(constructor)
void WINAPI
_init_init_lock(void) {
	/*
	 * If another constructor in the same binary is using mallctl to e.g.
	 * set up extent hooks, it may end up running before this one, and
	 * malloc_init_hard will crash trying to lock the uninitialized lock.
	 * So we force an initialization of the lock in malloc_init_hard as
	 * well. We don't try to care about atomicity of the access to the
	 * init_lock_initialized boolean, since it really only matters early
	 * in process creation, before any separate thread normally starts
	 * doing anything.
	 */
	if (!init_lock_initialized) {
		malloc_mutex_init(&init_lock, "init", WITNESS_RANK_INIT,
		    malloc_mutex_rank_exclusive);
	}
	init_lock_initialized = true;
}

#  ifdef _MSC_VER
#    pragma section(".CRT$XCU", read)
JEMALLOC_SECTION(".CRT$XCU")
JEMALLOC_ATTR(used)
static const void(WINAPI *init_init_lock)(void) = _init_init_lock;
#  endif
#endif
