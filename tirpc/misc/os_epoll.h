
#ifndef NTIRPC_OS_EPOLL_H
#define NTIRPC_OS_EPOLL_H

#if defined(__linux__)
# include <sys/epoll.h>
#else
/* epoll <-> kevent shim by Boaz Harrosh */
# include <misc/bsd_epoll.h>
#endif

#endif /* else NTIRPC_OS_EPOLL_H */
