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
 * Process identity. Fork-handler registration is added later.
 *
 * Capability flags: none.
 *
 * Functions:
 *   os_process_id() - current process id.
 * ==================================================================== */
JEMALLOC_ALWAYS_INLINE int
os_process_id(void) {
	return (int)GetCurrentProcessId();
}

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

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_H */
