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

#endif /* JEMALLOC_INTERNAL_OS_POSIX_COMMON_H */
