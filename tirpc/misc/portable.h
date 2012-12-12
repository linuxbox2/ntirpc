
#ifndef NTIRPC_PORTABLE_H
#define NTIRPC_PORTABLE_H

#include <misc/timespec.h>

#if defined(__FreeBSD__)
/* EPOLL->kevent shim by Boaz Harrosh */
#include <misc/epoll.h>
#include <netinet/in.h>

#define SOL_IP    0
#define SOL_IPV6  41

#define IP_PKTINFO IP_RECVIF

struct in_pktinfo {
  struct in_addr  ipi_addr;     /* destination IPv4 address */
  int             ipi_ifindex;  /* received interface index */
};

/* YES.  Move. */
#define HAVE_PEEREID 1

#endif

#if defined(__linux__)

#include <sys/epoll.h> /* before rpc.h */

/* POSIX clocks */
#define CLOCK_MONOTONIC_FAST CLOCK_MONOTONIC_COARSE

/* poll */
#define POLLRDNORM     0x040           /* Normal data may be read.  */
#define POLLRDBAND     0x080           /* Priority data may be read.  */

#define HAVE_GETPEEREID 0

#endif

#if defined(_WIN32)
#ifdef _MSC_VER

#ifdef _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <basetsd.h>
#include <misc/stdint.h>

#define MAXHOSTNAMELEN  64

#define __static_inline static __inline
#define __attribute_unused /* unused */

#define snprintf _snprintf
#define strtok_r strtok_s
#define strcasecmp _stricmp
#define getpid _getpid

#else
#include <stdint.h>

#define __static_inline static inline
#define __attribute_unused __attribute__((unused))

#endif
#include <reentrant.h>
#include <misc/timespec.h>

#define CLOCK_MONOTONIC_FAST 6

typedef uint32_t clockid_t;
extern int clock_gettime(clockid_t clock, struct timespec *ts);

void warnx(const char *fmt, ...);

#else
#define PtrToUlong(addr) ((unsigned long)(addr))
#endif /* !_WIN32 */

#ifdef __APPLE__
#include <sys/time.h>
typedef unsigned int clockid_t;
#define CLOCK_MONOTONIC_FAST 6
extern int clock_gettime(clockid_t clock, struct timespec *ts);
#endif

#ifndef max
#define max(a, b) (a > b ? a : b)
#endif

#if !defined(CACHE_LINE_SIZE)
#define CACHE_LINE_SIZE 64
#endif
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]

#endif /* NTIRPC_PORTABLE_H */
