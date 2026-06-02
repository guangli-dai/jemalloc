#ifndef JEMALLOC_INTERNAL_OS_SOLARIS_H
#define JEMALLOC_INTERNAL_OS_SOLARIS_H

#include "jemalloc/internal/nstime.h"

/*
 * Solaris OS layer.
 *
 * One section per OS facility, each self-contained: a short description, its
 * capability flags (OS_<FAC>_HAS_/_IS_/_CAN_<X>), and the functions it
 * provides. Inline bodies live here; out-of-line bodies live in
 * src/os/solaris.c.
 */

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
 *   os_time_monotonic(t) - best monotonic clock into *t (Solaris:
 *                          CLOCK_MONOTONIC, else gettimeofday).
 *   os_time_realtime(t)  - CLOCK_REALTIME wall clock into *t; unreachable()
 *                          if OS_TIME_HAS_REALTIME == 0.
 * ==================================================================== */
#include <time.h>
#ifdef JEMALLOC_HAVE_CLOCK_MONOTONIC
#  define OS_TIME_IS_MONOTONIC 1
#else
#  include <sys/time.h>
#  define OS_TIME_IS_MONOTONIC 0
#endif

#ifdef JEMALLOC_HAVE_CLOCK_REALTIME
#  define OS_TIME_HAS_REALTIME 1
#else
#  define OS_TIME_HAS_REALTIME 0
#endif

JEMALLOC_ALWAYS_INLINE void
os_time_monotonic(nstime_t *time) {
#ifdef JEMALLOC_HAVE_CLOCK_MONOTONIC
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	nstime_init2(time, ts.tv_sec, ts.tv_nsec);
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
 * Process
 *
 * Process identity. Fork-handler registration is added later.
 *
 * Capability flags: none.
 *
 * Functions:
 *   os_process_id() - current process id.
 * ==================================================================== */
#include <sys/types.h>
#include <unistd.h>

JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)getpid();
}

/* ====================================================================
 * File I/O
 *
 * Thin fd-based read/write/open/close used by malloc_io.c, prof_sys.c, and
 * the /proc + /sys readers. Direct syscalls where available.
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
 * Unsupported on Solaris: there are no /proc + /sys files this layer reads for
 * boot-time state.
 *
 * Capability flags: none.
 * Functions: none.
 * ==================================================================== */

#endif /* JEMALLOC_INTERNAL_OS_SOLARIS_H */
