#ifndef JEMALLOC_INTERNAL_OS_DARWIN_H
#define JEMALLOC_INTERNAL_OS_DARWIN_H

#include "jemalloc/internal/nstime.h"

/*
 * Darwin OS layer.
 *
 * One section per OS facility, each self-contained: a short description, its
 * capability flags (OS_<FAC>_HAS_/_IS_/_CAN_<X>), and the functions it
 * provides. Inline bodies live here; out-of-line bodies live in
 * src/os/darwin.c.
 */

/* ====================================================================
 * VM
 *
 * Darwin's VM facility is mmap-based, similar to other POSIX systems but
 * without madvise(MADV_HUGEPAGE) (no THP), without prctl/process_madvise, and
 * without dontdump or overcommit/THP introspection. Out-of-line bodies live in
 * src/os/darwin.c; the flags let portable src/pages.c drop whole branches at
 * compile time. State: `os_overcommits` (boot cache).
 *
 * Capability flags:
 *   OS_VM_HAS_HUGEPAGE        : os_vm_hugepage/nohugepage can hint THP.
 *   OS_VM_HAS_HUGE_COLLAPSE   : os_vm_huge_collapse is supported.
 *   OS_VM_HAS_BATCH_PURGE     : os_vm_batch_purge (process_madvise) works.
 *   OS_VM_HAS_PURGE_LAZY      : os_vm_purge_lazy reclaims lazily (MADV_FREE).
 *   OS_VM_HAS_PURGE_FORCED    : os_vm_purge_forced reclaims immediately.
 *   OS_VM_HAS_DONTDUMP        : os_vm_dontdump/dodump are supported.
 *   OS_VM_HAS_GUARD_PAGES     : os_vm_mark_guards/unmark_guards are supported.
 *   OS_VM_HAS_OVERCOMMIT_INFO : os_vm_overcommit_mode is meaningful.
 *   OS_VM_HAS_THP_INFO        : os_vm_thp_system_mode is meaningful.
 *   OS_VM_HAS_SET_NAME        : os_vm_set_name can name a mapping.
 *   OS_VM_CAN_PARTIAL_RELEASE : a sub-range can be released in place
 *                               (0 => pages.c must release + re-reserve).
 *   OS_VM_LAZY_PURGE_NEEDS_RUNTIME_CHECK : MADV_FREE must be probed at
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
 *   os_vm_purge_lazy(addr,size)            - lazy reclaim (MADV_FREE).
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

#ifdef JEMALLOC_PURGE_MADVISE_FREE
#  define OS_VM_HAS_PURGE_LAZY 1
#else
#  define OS_VM_HAS_PURGE_LAZY 0
#endif

#if (defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                  \
        && defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS))                     \
    || defined(JEMALLOC_MAPS_COALESCE)
#  define OS_VM_HAS_PURGE_FORCED 1
#else
#  define OS_VM_HAS_PURGE_FORCED 0
#endif

#define OS_VM_HAS_DONTDUMP 0
#define OS_VM_HAS_GUARD_PAGES 1
#define OS_VM_HAS_OVERCOMMIT_INFO 0
#define OS_VM_HAS_THP_INFO 0
#define OS_VM_HAS_SET_NAME 0

#define OS_VM_CAN_PARTIAL_RELEASE 1
#define OS_VM_LAZY_PURGE_NEEDS_RUNTIME_CHECK 1

/* Boot-time cache, set by os_vm_boot, read by os_vm_overcommit_mode. */
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
 * macOS 10.12+ exposes os_unfair_lock; jemalloc selects it via
 * JEMALLOC_OS_UNFAIR_LOCK at configure time and defines its own os_mutex_t
 * (os_unfair_lock) plus the os_mutex_* bodies inline here, rather than the
 * posix_common pthread mutex. os_unfair_lock has no destroy and no
 * postfork_child fixup beyond resetting to OS_UNFAIR_LOCK_INIT (it's a single
 * atomic word). Older macOS falls back to the posix_common pthread mutex
 * (OS_MUTEX_USE_PTHREAD), supplying only os_mutex_init locally.
 *
 * Capability flags:
 *   OS_MUTEX_USE_PTHREAD     : select the pthread mutex backend (posix_common.h);
 *                              defined only on the older-macOS fallback path.
 *   OS_MUTEX_HAS_STATIC_INIT : OS_MUTEX_INITIALIZER is a valid static initializer.
 *
 * Functions:
 *   os_mutex_lock(m)    - acquire the lock (void; os_unfair_lock path).
 *   os_mutex_unlock(m)  - release the lock (void; os_unfair_lock path).
 *   os_mutex_trylock(m) - try to acquire; true on failure (os_unfair_lock path).
 *   os_mutex_destroy(m) - no-op for os_unfair_lock (void; os_unfair_lock path).
 *   os_mutex_init(m)    - dynamically init a mutex; true on failure (defined on
 *                         both the os_unfair_lock and pthread-fallback paths).
 * ==================================================================== */
#ifdef JEMALLOC_OS_UNFAIR_LOCK
#  include <os/lock.h>
#  define OS_MUTEX_HAS_STATIC_INIT 1

typedef os_unfair_lock os_mutex_t;
#  define OS_MUTEX_INITIALIZER OS_UNFAIR_LOCK_INIT

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	os_unfair_lock_lock(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	os_unfair_lock_unlock(m);
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return !os_unfair_lock_trylock(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_destroy(os_mutex_t *m) {
	(void)m;
}

JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	*m = (os_unfair_lock)OS_UNFAIR_LOCK_INIT;
	return false;
}

#else /* fall back to pthread mutex on older macOS */
#  define OS_MUTEX_USE_PTHREAD
#  define OS_MUTEX_HAS_STATIC_INIT 1
#endif

/* ====================================================================
 * Cond + sigmask
 *
 * Defined only when background_thread support is compiled in
 * (JEMALLOC_BACKGROUND_THREAD), which is not the default on Darwin. When it is,
 * this section selects the posix_common pthread cond and sigset_t sigmask
 * backends and their inline bodies come from posix_common.h (included just
 * below); the cond is paired with a pthread_mutex_t the caller manages
 * separately (background_thread_info_t carries both fields today).
 *
 * Capability flags:
 *   OS_COND_USE_PTHREAD   : select the pthread cond backend (posix_common.h).
 *   OS_SIGMASK_USE_POSIX  : select the sigset_t sigmask backend (posix_common.h).
 *   OS_COND_HAS_TIMEDWAIT : os_cond_timedwait is available.
 *
 * Functions (posix_common.h, only when JEMALLOC_BACKGROUND_THREAD is set):
 *   os_cond_init/destroy/wait/timedwait/signal, os_sigmask_all_enter/leave.
 * ==================================================================== */
#ifdef JEMALLOC_BACKGROUND_THREAD
#  define OS_COND_USE_PTHREAD
#  define OS_SIGMASK_USE_POSIX
#  define OS_COND_HAS_TIMEDWAIT 1
#endif

#include "jemalloc/internal/os/posix_common.h"

#ifndef JEMALLOC_OS_UNFAIR_LOCK
JEMALLOC_ALWAYS_INLINE bool
os_mutex_init(os_mutex_t *m) {
	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		return true;
	}
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
	if (pthread_mutex_init(m, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return true;
	}
	pthread_mutexattr_destroy(&attr);
	return false;
}
#endif

/* ====================================================================
 * Time
 *
 * Monotonic and realtime clock reads, written into a caller-provided
 * nstime_t.
 *
 * Capability flags:
 *   OS_TIME_IS_MONOTONIC : os_time_monotonic is truly monotonic (0 => it
 *                          falls back to a wall clock).
 *   OS_TIME_HAS_REALTIME : os_time_realtime is callable on this OS.
 *
 * Functions:
 *   os_time_monotonic(t) - best monotonic clock into *t (Darwin:
 *                          clock_gettime_nsec_np(CLOCK_UPTIME_RAW),
 *                          else CLOCK_MONOTONIC, else mach_absolute_time,
 *                          else gettimeofday).
 *   os_time_realtime(t)  - CLOCK_REALTIME wall clock into *t; unreachable()
 *                          if OS_TIME_HAS_REALTIME == 0.
 * ==================================================================== */
#include <time.h>
#if defined(JEMALLOC_HAVE_CLOCK_GETTIME_NSEC_NP) \
    || defined(JEMALLOC_HAVE_CLOCK_MONOTONIC) \
    || defined(JEMALLOC_HAVE_MACH_ABSOLUTE_TIME)
#  define OS_TIME_IS_MONOTONIC 1
#else
#  include <sys/time.h>
#  define OS_TIME_IS_MONOTONIC 0
#endif

#ifdef JEMALLOC_HAVE_MACH_ABSOLUTE_TIME
#  include <mach/mach_time.h>
#endif

#ifdef JEMALLOC_HAVE_CLOCK_REALTIME
#  define OS_TIME_HAS_REALTIME 1
#else
#  define OS_TIME_HAS_REALTIME 0
#endif

JEMALLOC_ALWAYS_INLINE void
os_time_monotonic(nstime_t *time) {
#if defined(JEMALLOC_HAVE_CLOCK_GETTIME_NSEC_NP)
	nstime_init(time, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#elif defined(JEMALLOC_HAVE_CLOCK_MONOTONIC)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
#elif defined(JEMALLOC_HAVE_MACH_ABSOLUTE_TIME)
	static mach_timebase_info_data_t sTimebaseInfo;
	if (sTimebaseInfo.denom == 0) {
		(void)mach_timebase_info(&sTimebaseInfo);
	}
	nstime_init(time,
	    mach_absolute_time() * sTimebaseInfo.numer / sTimebaseInfo.denom);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	nstime_init2(time, tv.tv_sec, tv.tv_usec * 1000);
#endif
}

JEMALLOC_ALWAYS_INLINE void
os_time_realtime(nstime_t *time) {
#ifdef JEMALLOC_HAVE_CLOCK_REALTIME
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
#else
	unreachable();
#endif
}

/* ====================================================================
 * Thread
 *
 * OS-level thread identity. Darwin does not expose a gettid()-style kernel
 * thread id here, so os_thread_id() is not provided.
 *
 * Capability flags:
 *   OS_THREAD_HAS_GETTID : os_thread_id() is defined (backed by gettid());
 *                          0 on Darwin.
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
	long result = sysconf(_SC_NPROCESSORS_ONLN);
	return (result == -1) ? 1u : (unsigned)result;
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_count_is_deterministic(void) {
	long cpu_onln = sysconf(_SC_NPROCESSORS_ONLN);
	long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
	return cpu_onln == cpu_conf;
}

JEMALLOC_ALWAYS_INLINE int
os_cpu_current(void) {
#if defined(__aarch64__)
	uintptr_t c;
	asm volatile("mrs %x0, tpidrro_el0" : "=r"(c)::"memory");
	return (int)(c & ((1 << 3) - 1));
#elif defined(JEMALLOC_HAVE_RDTSCP)
	unsigned int ecx;
	asm volatile("rdtscp" : "=c"(ecx)::"eax", "edx");
	return (int)(ecx & 0xfff);
#else
	return -1;
#endif
}

/* ====================================================================
 * Process
 *
 * Process id and fork-handler registration. On Darwin the malloc-zone fork
 * callbacks subsume pthread_atfork, so os_process_register_atfork is a no-op
 * that returns false. See zone.c (folded into darwin.c).
 *
 * Capability flags:
 *   OS_PROCESS_HAS_ATFORK : os_process_register_atfork installs hooks (0 =>
 *                           it is a no-op; on Darwin it returns false).
 *
 * Functions:
 *   os_process_id()                         - getpid().
 *   os_process_register_atfork(pre,par,chld) - install fork handlers; true
 *                                             on failure (no-op returning false
 *                                             on Darwin).
 * ==================================================================== */
#define OS_PROCESS_HAS_ATFORK 0

#include <sys/types.h>
#include <unistd.h>

JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)getpid();
}

bool os_process_register_atfork(void (*prepare)(void),
                                void (*parent)(void),
                                void (*child)(void));

/* ====================================================================
 * File I/O
 *
 * Thin fd-based read/write/open/close used by malloc_io.c and prof_sys.c.
 * Direct syscalls where available.
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
#include <errno.h>
#include <fcntl.h>
#ifdef JEMALLOC_USE_SYSCALL
#  include <sys/syscall.h>
#endif

#define OS_FILE_RETRIES_EINTR 1

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_write_once(int fd, const void *buf, size_t count) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_write)
	return (ssize_t)syscall(SYS_write, fd, buf, count);
#else
	return (ssize_t)write(fd, buf, count);
#endif
}

JEMALLOC_ALWAYS_INLINE ssize_t
os_file_read_once(int fd, void *buf, size_t count) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_read)
	return (ssize_t)syscall(SYS_read, fd, buf, count);
#else
	return (ssize_t)read(fd, buf, count);
#endif
}

JEMALLOC_ALWAYS_INLINE bool
os_file_interrupted(void) {
	return errno == EINTR;
}

JEMALLOC_ALWAYS_INLINE int
os_file_open(const char *path, int flags) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_open)
	return (int)syscall(SYS_open, path, flags);
#elif defined(JEMALLOC_USE_SYSCALL) && defined(SYS_openat)
	return (int)syscall(SYS_openat, AT_FDCWD, path, flags);
#else
	return open(path, flags);
#endif
}

JEMALLOC_ALWAYS_INLINE int
os_file_close(int fd) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_close)
	return (int)syscall(SYS_close, fd);
#else
	return close(fd);
#endif
}

JEMALLOC_ALWAYS_INLINE off_t
os_file_lseek(int fd, off_t offset, int whence) {
#if defined(JEMALLOC_USE_SYSCALL) && defined(SYS_lseek)
	return (off_t)syscall(SYS_lseek, fd, offset, whence);
#else
	return lseek(fd, offset, whence);
#endif
}

/* ====================================================================
 * /proc and /sys reads
 *
 * Linux-only kernel introspection (overcommit / THP state). Unsupported on
 * Darwin: there is no /proc or /sys to read.
 *
 * Capability flags: none.
 * Functions: none.
 * ==================================================================== */

#endif /* JEMALLOC_INTERNAL_OS_DARWIN_H */
