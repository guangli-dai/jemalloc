#ifndef JEMALLOC_INTERNAL_OS_FREEBSD_H
#define JEMALLOC_INTERNAL_OS_FREEBSD_H

#include "jemalloc/internal/nstime.h"

/*
 * FreeBSD OS layer.
 *
 * One section per OS facility, each self-contained: a short description, its
 * capability flags (OS_<FAC>_HAS_/_IS_/_CAN_<X>), and the functions it
 * provides. Inline bodies live here; out-of-line bodies live in
 * src/os/freebsd.c.
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
 *   os_time_monotonic(t) - best monotonic clock into *t (FreeBSD:
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

#endif /* JEMALLOC_INTERNAL_OS_FREEBSD_H */
