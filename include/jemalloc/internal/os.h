#ifndef JEMALLOC_INTERNAL_OS_H
#define JEMALLOC_INTERNAL_OS_H

/*
 * OS layer dispatcher.
 *
 * Portable code that needs an OS-touching primitive includes this header,
 * which selects the matching per-OS bundle. Each bundle (os/<os>.h) is
 * organized as one self-contained section per OS facility: a description,
 * the facility's capability flags (OS_<FAC>_HAS_/_IS_/_CAN_<X>), and the
 * functions it provides.
 *
 * Adding a new OS: drop in include/jemalloc/internal/os/<os>.h plus the
 * matching src/os/<os>.c, then extend the case statement here and the
 * OS_LAYER detection in configure.ac.
 */

#if defined(__linux__)
#  include "jemalloc/internal/os/linux.h"
#elif defined(__APPLE__)
#  include "jemalloc/internal/os/darwin.h"
#elif defined(_WIN32)
#  include "jemalloc/internal/os/windows.h"
#elif defined(__FreeBSD__)
#  include "jemalloc/internal/os/freebsd.h"
#elif defined(__NetBSD__)
#  include "jemalloc/internal/os/netbsd.h"
#elif defined(__sun)
#  include "jemalloc/internal/os/solaris.h"
#else
#  error "unsupported OS - add a corresponding os/<os>.h"
#endif

#endif /* JEMALLOC_INTERNAL_OS_H */
