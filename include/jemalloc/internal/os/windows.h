#ifndef JEMALLOC_INTERNAL_OS_WINDOWS_H
#define JEMALLOC_INTERNAL_OS_WINDOWS_H

#include "jemalloc/internal/nstime.h"

/*
 * Windows OS layer.
 *
 * One section per OS facility, each self-contained: a short description, its
 * capability flags (OS_<FAC>_HAS_/_IS_/_CAN_<X>), and the functions it
 * provides. Inline bodies live here; out-of-line bodies live in
 * src/os/windows.c.
 */

/* ====================================================================
 * VM
 *
 * Windows's VM facility: VirtualAlloc/VirtualFree. VirtualFree(MEM_RELEASE)
 * is all-or-nothing — partial releases are not supported — so the
 * over-allocate-and-trim slow path in src/pages.c releases the whole
 * reservation and re-reserves the trimmed region (OS_VM_CAN_PARTIAL_RELEASE
 * = 0 selects that branch). Lazy purge is MEM_RESET; forced purge, hugepages,
 * batch purge, dontdump, naming, and overcommit/THP info are unsupported.
 * State: `os_overcommits` (boot cache).
 *
 * Capability flags:
 *   OS_VM_HAS_HUGEPAGE        : os_vm_hugepage/nohugepage can hint THP.
 *   OS_VM_HAS_HUGE_COLLAPSE   : os_vm_huge_collapse is supported.
 *   OS_VM_HAS_BATCH_PURGE     : os_vm_batch_purge (process_madvise) works.
 *   OS_VM_HAS_PURGE_LAZY      : os_vm_purge_lazy reclaims lazily (MEM_RESET).
 *   OS_VM_HAS_PURGE_FORCED    : os_vm_purge_forced reclaims immediately.
 *   OS_VM_HAS_DONTDUMP        : os_vm_dontdump/dodump are supported.
 *   OS_VM_HAS_GUARD_PAGES     : os_vm_mark_guards/unmark_guards are supported.
 *   OS_VM_HAS_OVERCOMMIT_INFO : os_vm_overcommit_mode is meaningful.
 *   OS_VM_HAS_THP_INFO        : os_vm_thp_system_mode is meaningful.
 *   OS_VM_HAS_SET_NAME        : os_vm_set_name can name a mapping.
 *   OS_VM_CAN_PARTIAL_RELEASE : a sub-range can be released in place
 *                               (0 => pages.c must release + re-reserve).
 *   OS_VM_LAZY_PURGE_NEEDS_RUNTIME_CHECK : MEM_RESET must be probed at
 *                               runtime by pages_boot.
 *
 * Functions (bool returns are true-on-failure unless noted):
 *   os_vm_overcommit_mode()                - cached overcommit policy (int).
 *   os_vm_page_size()                      - system page size in bytes.
 *   os_vm_boot()                           - one-time init.
 *   os_vm_reserve(hint,size,align,*commit) - reserve space; base or NULL,
 *                                            may clear *commit.
 *   os_vm_release(addr,size)               - unmap a reservation (void).
 *   os_vm_commit(addr,size)                - back a range with memory.
 *   os_vm_decommit(addr,size)              - drop backing, keep reservation.
 *   os_vm_mark_guards(head,tail)           - protect guard pages (void).
 *   os_vm_unmark_guards(head,tail)         - undo mark_guards (void).
 *   os_vm_purge_lazy(addr,size)            - lazy reclaim (MEM_RESET).
 *   os_vm_purge_forced(addr,size)          - immediate reclaim.
 *   os_vm_purge_forced_is_enabled()        - true if forced purge is usable.
 *   os_vm_batch_purge(vec,vec_len,total)   - purge many ranges at once.
 *   os_vm_hugepage(addr,size)              - hint huge pages.
 *   os_vm_nohugepage(addr,size)            - undo hugepage hint.
 *   os_vm_huge_collapse(addr,size)         - request synchronous THP collapse.
 *   os_vm_dontdump(addr,size)              - exclude range from core dumps.
 *   os_vm_dodump(addr,size)                - undo dontdump.
 *   os_vm_set_name(addr,size,name)         - name a mapping (debugging).
 *   os_vm_thp_system_mode()                - system THP mode (int).
 * ==================================================================== */
#define OS_VM_HAS_HUGEPAGE 0
#define OS_VM_HAS_HUGE_COLLAPSE 0
#define OS_VM_HAS_BATCH_PURGE 0
#define OS_VM_HAS_PURGE_LAZY 1
/* See pages.h note: PAGES_CAN_PURGE_FORCED is off on Windows because
 * forced purge would require decommit+recommit, which is racy. */
#define OS_VM_HAS_PURGE_FORCED 0
#define OS_VM_HAS_DONTDUMP 0
#define OS_VM_HAS_GUARD_PAGES 1
#define OS_VM_HAS_OVERCOMMIT_INFO 0
#define OS_VM_HAS_THP_INFO 0
#define OS_VM_HAS_SET_NAME 0

#define OS_VM_CAN_PARTIAL_RELEASE 0
#define OS_VM_LAZY_PURGE_NEEDS_RUNTIME_CHECK 1

extern bool os_overcommits;

JEMALLOC_ALWAYS_INLINE int
os_vm_overcommit_mode(void) {
	return (int)os_overcommits;
}

size_t os_vm_page_size(void);
bool   os_vm_boot(void);

void  *os_vm_reserve(void *hint, size_t size, size_t align, bool *commit);
void   os_vm_release(void *addr, size_t size);
bool   os_vm_commit(void *addr, size_t size);
bool   os_vm_decommit(void *addr, size_t size);

void   os_vm_mark_guards(void *head, void *tail);
void   os_vm_unmark_guards(void *head, void *tail);

bool   os_vm_purge_lazy(void *addr, size_t size);
bool   os_vm_purge_forced(void *addr, size_t size);
bool   os_vm_purge_forced_is_enabled(void);
bool   os_vm_batch_purge(void *vec, size_t vec_len, size_t total_bytes);

bool   os_vm_hugepage(void *addr, size_t size);
bool   os_vm_nohugepage(void *addr, size_t size);
bool   os_vm_huge_collapse(void *addr, size_t size);

bool   os_vm_dontdump(void *addr, size_t size);
bool   os_vm_dodump(void *addr, size_t size);

bool   os_vm_set_name(void *addr, size_t size, const char *name);

int    os_vm_thp_system_mode(void);

/* ====================================================================
 * Sync (mutex)
 *
 * Windows defines its own os_mutex_t and inline bodies here (it does not use
 * posix_common.h). On Vista+ os_mutex_t is SRWLOCK (lighter than
 * CRITICAL_SECTION); older targets fall back to CRITICAL_SECTION with a spin
 * count. malloc_mutex_t embeds os_mutex_t as its `lock` field;
 * MALLOC_MUTEX_INITIALIZER stays empty (the lock is initialized at runtime via
 * os_mutex_init from malloc_mutex_init / malloc_mutex_boot).
 *
 * Capability flags:
 *   OS_MUTEX_HAS_STATIC_INIT : OS_MUTEX_INITIALIZER is a valid static initializer.
 *
 * Functions:
 *   os_mutex_init(m) - dynamically init a mutex; true on failure.
 *   os_mutex_lock/unlock/trylock/destroy - acquire/release/try/teardown.
 * ==================================================================== */
#if _WIN32_WINNT >= 0x0600
typedef SRWLOCK os_mutex_t;
/*
 * MALLOC_MUTEX_INITIALIZER stays empty on Windows: jemalloc_init.c uses a
 * direct SRWLOCK_INIT for init_lock, and tsd.c never expands the macro on
 * Windows. Keeping OS_MUTEX_HAS_STATIC_INIT 0 preserves that.
 */
#  define OS_MUTEX_HAS_STATIC_INIT 0

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	AcquireSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	ReleaseSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !TryAcquireSRWLockExclusive(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_destroy(os_mutex_t *m) {
	(void)m;
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	InitializeSRWLock(m);
	return false;
}
#else
typedef CRITICAL_SECTION os_mutex_t;
#  define OS_MUTEX_HAS_STATIC_INIT 0

#  ifndef _CRT_SPINCOUNT
#    define _CRT_SPINCOUNT 4000
#  endif

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	EnterCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	LeaveCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !TryEnterCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_destroy(os_mutex_t *m) {
	DeleteCriticalSection(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	return !InitializeCriticalSectionAndSpinCount(m, _CRT_SPINCOUNT);
}
#endif

/* ====================================================================
 * Cond + sigmask
 *
 * Windows has no sigmask, and it does not enable JEMALLOC_BACKGROUND_THREAD
 * today, so the cond + sigmask facility is effectively unused. Reserved for a
 * future native port via CONDITION_VARIABLE / SleepConditionVariableSRW; no
 * cond/sigmask functions are provided here.
 *
 * Capability flags:
 *   OS_COND_HAS_TIMEDWAIT : os_cond_timedwait is available.
 *
 * Functions: none.
 * ==================================================================== */
#define OS_COND_HAS_TIMEDWAIT 0

/* ====================================================================
 * Time
 *
 * Clock reads into a caller-provided nstime_t. Windows has no cheap monotonic
 * clock here: os_time_monotonic uses GetSystemTimeAsFileTime, which is a WALL
 * clock, so OS_TIME_IS_MONOTONIC is 0 (the "monotonic" reader is really
 * wall-clock). os_time_realtime is unreachable() (OS_TIME_HAS_REALTIME 0).
 *
 * Capability flags:
 *   OS_TIME_IS_MONOTONIC : os_time_monotonic is truly monotonic (0 => it
 *                          falls back to a wall clock).
 *   OS_TIME_HAS_REALTIME : os_time_realtime is callable on this OS.
 *
 * Functions:
 *   os_time_monotonic(t) - best monotonic clock into *t (Windows: wall-clock
 *                          GetSystemTimeAsFileTime).
 *   os_time_realtime(t)  - CLOCK_REALTIME wall clock into *t; unreachable()
 *                          if OS_TIME_HAS_REALTIME == 0.
 * ==================================================================== */
#define OS_TIME_IS_MONOTONIC 0
#define OS_TIME_HAS_REALTIME 0

JEMALLOC_ALWAYS_INLINE void
os_time_monotonic(nstime_t *time) {
	FILETIME ft;
	uint64_t ticks_100ns;

	GetSystemTimeAsFileTime(&ft);
	ticks_100ns = (((uint64_t)ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

	nstime_init(time, ticks_100ns * 100);
}

JEMALLOC_ALWAYS_INLINE void
os_time_realtime(nstime_t *time) {
	unreachable();
}

/* ====================================================================
 * Thread
 *
 * OS-level thread identity. Windows does not expose a gettid()-style kernel
 * thread id here, so OS_THREAD_HAS_GETTID is 0 and no os_thread_id is provided.
 *
 * Capability flags:
 *   OS_THREAD_HAS_GETTID : os_thread_id() is defined (backed by gettid()).
 *
 * Functions: none.
 * ==================================================================== */
#define OS_THREAD_HAS_GETTID 0

/* ====================================================================
 * CPU
 *
 * CPU counts and current-CPU queries used for arena / tcache sizing.
 *
 * Capability flags: none.
 *
 * Functions:
 *   os_cpu_ncpus()                  - number of usable CPUs (>= 1).
 *   os_cpu_count_is_deterministic() - false if the CPU count can change at
 *                                     runtime (affects caching decisions).
 *   os_cpu_current()                - current CPU index, or -1 if unknown.
 * ==================================================================== */
JEMALLOC_ALWAYS_INLINE unsigned
os_cpu_ncpus(void) {
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (unsigned)si.dwNumberOfProcessors;
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_count_is_deterministic(void) {
	return true;
}

JEMALLOC_ALWAYS_INLINE int
os_cpu_current(void) {
	return (int)GetCurrentProcessorNumber();
}

/* ====================================================================
 * Process
 *
 * Process id and fork-handler registration. Windows has no fork(2) or
 * pthread_atfork (CreateProcess does not duplicate the address space the way
 * fork(2) does), so OS_PROCESS_HAS_ATFORK is 0 and os_process_register_atfork
 * is a no-op (returns false).
 *
 * Capability flags:
 *   OS_PROCESS_HAS_ATFORK : os_process_register_atfork installs hooks (0 =>
 *                           it is a no-op).
 *
 * Functions:
 *   os_process_id()                          - GetCurrentProcessId().
 *   os_process_register_atfork(pre,par,chld) - install fork handlers; no-op
 *                                              that returns false on Windows.
 * ==================================================================== */
#define OS_PROCESS_HAS_ATFORK 0

JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)GetCurrentProcessId();
}

bool os_process_register_atfork(void (*prepare)(void),
                                void (*parent)(void),
                                void (*child)(void));

/* ====================================================================
 * File I/O
 *
 * Thin fd-based read/write/open/close used by malloc_io.c and prof_sys.c,
 * backed by the C runtime io.h (_read/_write/_open/_close). Windows never
 * fails with EINTR, so OS_FILE_RETRIES_EINTR is 0.
 *
 * Capability flags:
 *   OS_FILE_RETRIES_EINTR : os_file_interrupted() can return true, i.e.
 *                           callers must retry *_once() on EINTR.
 *
 * Functions:
 *   os_file_open(path,flags)     - open a file; fd or -1.
 *   os_file_close(fd)            - close; 0 on success.
 *   os_file_lseek(fd,off,whence) - reposition file offset; new offset or -1.
 *   os_file_read_once(fd,buf,n)  - one read();  bytes read or -1.
 *   os_file_write_once(fd,buf,n) - one write(); bytes written or -1.
 *   os_file_interrupted()        - true if the last call failed with EINTR.
 * ==================================================================== */
#include <io.h>
#include <fcntl.h>

#define OS_FILE_RETRIES_EINTR 0

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write_once(int fd, const void *buf, size_t count) {
	return (ssize_t)write(fd, buf, (unsigned int)count);
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_read_once(int fd, void *buf, size_t count) {
	return (ssize_t)read(fd, buf, (unsigned int)count);
}

JEMALLOC_ALWAYS_INLINE bool
os_file_interrupted(void) {
	return false;
}

JEMALLOC_ALWAYS_INLINE int
os_file_open(const char *path, int flags) {
	return open(path, flags);
}

JEMALLOC_ALWAYS_INLINE int
os_file_close(int fd) {
	return close(fd);
}

JEMALLOC_ALWAYS_INLINE off_t
os_file_lseek(int fd, off_t offset, int whence) {
	return lseek(fd, offset, whence);
}

/* ====================================================================
 * /proc and /sys reads
 *
 * Linux-style /proc + /sys pseudo-files do not exist on Windows; this facility
 * is unsupported and nothing reads them here.
 *
 * Capability flags: none.
 * Functions: none.
 * ==================================================================== */

/* ====================================================================
 * DSS / sbrk
 *
 * Data-segment growth via sbrk(2), used by src/extent_dss.c. Windows has no
 * sbrk(2), so DSS is unsupported: OS_DSS_HAS_SBRK is always 0 and os_dss_sbrk
 * is declared but unreachable.
 *
 * Capability flags:
 *   OS_DSS_HAS_SBRK : os_dss_sbrk is usable (always 0 on Windows).
 *
 * Functions:
 *   os_dss_sbrk(increment) - move the program break; unreachable on Windows.
 * ==================================================================== */
#define OS_DSS_HAS_SBRK 0

void *os_dss_sbrk(intptr_t increment);

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_H */
