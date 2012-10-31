
#ifndef NTIRPC_PORTABLE_H
#define NTIRPC_PORTABLE_H

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
#else
#define PtrToUlong(addr) ((unsigned long)(addr))
#endif

#if !defined(CACHE_LINE_SIZE)
#define CACHE_LINE_SIZE 64
#endif
#define CACHE_PAD(_n) char __pad ## _n [CACHE_LINE_SIZE]

#endif /* NTIRPC_PORTABLE_H */
