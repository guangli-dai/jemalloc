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

#endif /* JEMALLOC_INTERNAL_OS_WINDOWS_H */
