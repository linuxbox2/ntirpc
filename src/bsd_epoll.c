/*
 * bsd_epoll.c - Linux epoll.h emulation via kevent API
 *
 * Copyright (c) 2009 Boaz Harrosh <bharrosh@panasas.com>
 * All rights reserved.
 *
 * Inspired by code written by Roman Divacky
 * from the freebsd's linux emulation project at linux_epoll.c file
 * However none of that code ended up here.
 *
 * description:
 *    This file implements a workable set of the <sys/epoll.h> header file
 *    on a linux distribution. The implementation translates back and forth from
 *    epoll calls and constants to Kevent calls and constants.
 *
 *   This file is copyrighted under the "New BSD License" (see below) so it can
 *   be included in the tirpc library project.
 *   But I fully expect that if you make any fixes/enhancements to bsd_epoll.c
 *   you shall send these changes to me for inclusion in the next version.
 *   (Or I'll hunt your dreams, and you will not have peace)
 *
 * License: "New BSD License"
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the <organization> nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#if defined(__FreeBSD__)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <misc/bsd_epoll.h>

/* Create a new epoll file descriptor. */
int epoll_create(int size)
{
	if (size <= 0)
		return -EINVAL;
	/*
	 * args->size is unused. Linux just tests it
	 * and then forgets it as well.
	 */

	return kqueue();
}

static void
kevent_to_epoll(struct kevent *kevent, struct epoll_event *event)
{
	/* if (kevent->flags & EV_ERROR) {
		... Any special event flags on Errors?
	} */

	memset(event, 0, sizeof(*event));
	switch (kevent->filter) {
	case EVFILT_READ:
		/* in Linux EPOLLIN with Zero Data will signal a close
		 * condition. So do that if EV_EOF.
		 */
		if ((kevent->data > 0) || (kevent->flags & EV_EOF))
			event->events = EPOLLIN;
		break;
	case EVFILT_WRITE:
		if (kevent->data > 0)
			event->events = EPOLLOUT;
		break;
	default:
		fprintf(stderr, "Unsupported kevent_to_epoll event=%d"
			" data=%lx flags=0x%x ident=%ld fflags=0x%x\n",
			kevent->filter, (long)kevent->data,
			kevent->flags, (long)kevent->ident,
			kevent->fflags);
	}
	event->data.ptr = kevent->udata;
}

static void kevent_to_epoll_arr(struct kevent *kevp, int count,
				struct epoll_event *eep)
{
	int i;

	for (i = 0; i < count; i++) {
		kevent_to_epoll(&kevp[i], &eep[i]);
		/*printf("eep[%d]=%p\n", i, eep[i].data.ptr);*/
	}
}

/*
 * Always create two filters An EVFILT_READ and an EVFILT_WRITE.
 * EV_ENABLE/EV_DISABLE according to epoll flags. On EPOLL_CTL_MOD
 * first delete then add a new. On EPOLL_CTL_DEL remove both.
 */
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	struct kevent kev[2] = {{.flags = 0},};
	int read_flags, write_flags;

	switch (op) {
	case EPOLL_CTL_MOD:
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, event);
		/* fall through */
	case EPOLL_CTL_ADD:
	{
		int flags;

		if ((event->events & EPOLLIN) || event->events & EPOLLPRI)
			read_flags = EV_ENABLE;
		else
			read_flags = EV_DISABLE;

		if (event->events & EPOLLOUT)
			write_flags = EV_ENABLE;
		else
			write_flags = EV_DISABLE;

		flags = EV_ADD;
		if (event->events & EPOLLET)
			flags |= EV_CLEAR;
		if (event->events & EPOLLONESHOT)
			flags |= EV_ONESHOT;

		read_flags |= flags; write_flags |= flags;
/*		printf("%s(fd=%d data.ptr=%p\n",
			op == EPOLL_CTL_MOD ? "EPOLL_CTL_MOD" : "EPOLL_CTL_ADD",
			fd ,event->data.ptr);
		printf("    EVFILT_READ(on=%d read_flags=0x%x\n",
			read_flags & EV_ENABLE, read_flags);
		printf("    EVFILT_WRITE(on=%d write_flags=0x%x);\n",
			write_flags & EV_ENABLE, write_flags);*/
	}
		break;
	case EPOLL_CTL_DEL:
		read_flags = write_flags = EV_DELETE | EV_DISABLE;
		/*printf("EPOLL_CTL_DEL(fd=%d\n",fd);*/
		break;
	default:
		fprintf(stderr, "Unsupported epoll operation=%d"
			" epfd=%d, fd=%d events=%x\n",
			op, epfd, fd, event->events);
		errno = EINVAL;
		return -1;
	}

	EV_SET(&kev[0], fd, EVFILT_READ, read_flags, 0, 0,
		event ? event->data.ptr : 0);
	EV_SET(&kev[1], fd, EVFILT_WRITE, write_flags, 0, 0,
		event ? event->data.ptr : 0);

	return kevent(epfd, kev, 2, NULL, 0, NULL);
}

/*
 * Wait for a filter to be triggered on the epoll file descriptor.
 */
int epoll_wait(int epfd, struct epoll_event *events,
		       int maxevents, int timeout)
{
	struct kevent *kevp;
	struct timespec ts;
	int rc;

	/* Convert from miliseconds to timespec. */
	ts.tv_sec = timeout / 1000000;
	ts.tv_nsec = (timeout % 1000000) * 1000;

	kevp = calloc(maxevents, sizeof(*kevp));
	/*
	 * ENOMEM is not expected from epoll_wait.
	 * Maybe we should translate that but I don't think it matters at all.
	 */
	if (!kevp)
		return -ENOMEM;

	rc = kevent(epfd, NULL, 0, kevp, maxevents, &ts);

	if (rc > 0) {
		/*printf("epoll_wait LEAVE %d\n", rc);*/
		kevent_to_epoll_arr(kevp, rc, events);
	}

	free(kevp);
	return rc;
}

#endif /* __FreeBSD__ */
