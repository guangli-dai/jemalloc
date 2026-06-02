#ifndef JEMALLOC_INTERNAL_OS_POSIX_COMMON_H
#define JEMALLOC_INTERNAL_OS_POSIX_COMMON_H

/*
 * Shared POSIX inline helpers used by Linux, Darwin, FreeBSD, NetBSD, and
 * Solaris bundles. Per-OS headers #include this for sections that don't
 * need OS-specific behavior (cond/mutex/time/pages bodies). Per-OS
 * headers override what they need (e.g., Darwin replaces the pthread
 * mutex section with os_unfair_lock).
 *
 * Windows does NOT include this header.
 */

#include <pthread.h>
#include <signal.h>
#include <time.h>

/* ====================================================================
 * Sync (mutex) - POSIX pthread backend
 *
 * Compiled only when the including bundle defines OS_MUTEX_USE_PTHREAD
 * before including this header. Darwin defines its own os_mutex_t
 * (os_unfair_lock) and skips this block. os_mutex_init is supplied by each
 * per-OS bundle, not here.
 *
 * Gate:
 *   OS_MUTEX_USE_PTHREAD : include this pthread mutex backend.
 *
 * Functions:
 *   os_mutex_t           - typedef (pthread_mutex_t).
 *   OS_MUTEX_INITIALIZER - static initializer for os_mutex_t.
 *   os_mutex_lock(m)     - lock (void).
 *   os_mutex_unlock(m)   - unlock (void).
 *   os_mutex_trylock(m)  - try-lock; true on failure (lock not acquired).
 *   os_mutex_destroy(m)  - destroy (void).
 * ==================================================================== */
#ifdef OS_MUTEX_USE_PTHREAD

typedef pthread_mutex_t os_mutex_t;
#define OS_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

JEMALLOC_ALWAYS_INLINE void
os_mutex_lock(os_mutex_t *m) {
	pthread_mutex_lock(m);
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_unlock(os_mutex_t *m) {
	pthread_mutex_unlock(m);
}

/* Returns true on failure (matches MALLOC_MUTEX_TRYLOCK semantics). */
JEMALLOC_ALWAYS_INLINE bool
os_mutex_trylock(os_mutex_t *m) {
	return pthread_mutex_trylock(m) != 0;
}

JEMALLOC_ALWAYS_INLINE void
os_mutex_destroy(os_mutex_t *m) {
	pthread_mutex_destroy(m);
}

#endif /* OS_MUTEX_USE_PTHREAD */

/* ====================================================================
 * Cond + sigmask
 *
 * POSIX pthread condvar wrapper plus a sigset_t-based signal-mask helper,
 * each gated by its own selector. Darwin overrides the cond section (its
 * cond pairs with os_unfair_lock rather than a pthread_mutex_t, so it
 * cannot use the pthread_cond_wait signature).
 *
 * Gates:
 *   OS_COND_USE_PTHREAD  : include the pthread cond backend.
 *   OS_SIGMASK_USE_POSIX : include the sigset_t sigmask backend.
 *
 * Functions (cond):
 *   os_cond_t                  - typedef (pthread_cond_t).
 *   os_cond_init(c)            - init; true on failure.
 *   os_cond_destroy(c)         - destroy (void).
 *   os_cond_wait(c,m)          - wait; returns errno (0 on wakeup).
 *   os_cond_timedwait(c,m,abs) - wait until *abs (CLOCK_REALTIME deadline);
 *                                0 on wakeup, ETIMEDOUT on timeout.
 *   os_cond_signal(c)          - wake one waiter (void).
 * Functions (sigmask):
 *   os_sigmask_t                - typedef (sigset_t).
 *   os_sigmask_all_enter(saved) - mask all signals, save prior; returns errno.
 *   os_sigmask_leave(saved)     - restore a saved mask; returns errno.
 * ==================================================================== */
#ifdef OS_COND_USE_PTHREAD

typedef pthread_cond_t os_cond_t;

JEMALLOC_ALWAYS_INLINE bool
os_cond_init(os_cond_t *c) {
	return pthread_cond_init(c, NULL) != 0;
}

JEMALLOC_ALWAYS_INLINE void
os_cond_destroy(os_cond_t *c) {
	pthread_cond_destroy(c);
}

JEMALLOC_ALWAYS_INLINE int
os_cond_wait(os_cond_t *c, os_mutex_t *m) {
	return pthread_cond_wait(c, m);
}

/*
 * abs_ts is an absolute CLOCK_REALTIME deadline. Returns 0 on signal,
 * ETIMEDOUT on timeout, other errno otherwise.
 */
JEMALLOC_ALWAYS_INLINE int
os_cond_timedwait(os_cond_t *c, os_mutex_t *m, const struct timespec *abs_ts) {
	return pthread_cond_timedwait(c, m, abs_ts);
}

JEMALLOC_ALWAYS_INLINE void
os_cond_signal(os_cond_t *c) {
	pthread_cond_signal(c);
}

#endif /* OS_COND_USE_PTHREAD */

#ifdef OS_SIGMASK_USE_POSIX

typedef sigset_t os_sigmask_t;

/* Mask all signals, returning the prior mask in *saved. */
JEMALLOC_ALWAYS_INLINE int
os_sigmask_all_enter(os_sigmask_t *saved) {
	sigset_t set;
	sigfillset(&set);
	return pthread_sigmask(SIG_SETMASK, &set, saved);
}

JEMALLOC_ALWAYS_INLINE int
os_sigmask_leave(const os_sigmask_t *saved) {
	return pthread_sigmask(SIG_SETMASK, saved, NULL);
}

#endif /* OS_SIGMASK_USE_POSIX */

#endif /* JEMALLOC_INTERNAL_OS_POSIX_COMMON_H */
