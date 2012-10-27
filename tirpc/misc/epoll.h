
#ifndef NTIRPC_EPOLL_H
#define NTIRPC_EPOLL_H

#if defined(__linux__)
# include <sys/epoll.h>
#else

/* Boaz will supply this section RSN. */

#endif /* !__linux__ */
#endif /* NTIRPC_EPOLL_H */
