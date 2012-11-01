
#ifndef NTIRPC_EPOLL_H
#define NTIRPC_EPOLL_H

#if defined(__linux__)
# include <sys/epoll.h>
#else

/*-
 * Copyright (c) 2007 Roman Divacky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef _LINUX_EPOLL_H_
#define _LINUX_EPOLL_H_

#ifdef __amd64__
#define EPOLL_PACKED    __packed
#else
#define EPOLL_PACKED
#endif

union epoll_data
{
  int fd;
  uint32_t u32;
  uint64_t u64;
  void *ptr;
};

struct epoll_event {
        uint32_t events;
        union epoll_data data;
} EPOLL_PACKED;

#define EPOLLIN           0x001
#define EPOLLPRI          0x002
#define EPOLLOUT          0x004
#define EPOLLONESHOT      (1 << 30)
#define EPOLLET           (1 << 31)

#define EPOLL_CTL_ADD     1
#define EPOLL_CTL_DEL     2
#define EPOLL_CTL_MOD     3

#define LINUX_MAX_EVENTS        (INT_MAX / sizeof(struct linux_epoll_event))

extern int epoll_create(int);
extern int epoll_ctl(int, int, int, struct epoll_event *);
extern int epoll_wait(int, struct epoll_event *, int, int);

#endif  /* !_LINUX_EPOLL_H_ */

#endif /* !__linux__ */
#endif /* NTIRPC_EPOLL_H */
