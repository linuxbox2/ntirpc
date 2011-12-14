/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is the rpc server side idle loop
 * Wait for input, call server program.
 */
#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(TIRPC_EPOLL)
#include <sys/epoll.h> /* before rpc.h */
#endif
#include <rpc/rpc.h>
#include "rpc_com.h"
#include <sys/select.h>

extern svc_params __svc_params[1];

static void
svc_run_select()
{
	fd_set readfds, cleanfds;
	struct timeval timeout;
	extern rwlock_t svc_fd_lock;


	for (;;) {
		rwlock_rdlock(&svc_fd_lock);
		readfds = svc_fdset;
		cleanfds = svc_fdset;
		rwlock_unlock(&svc_fd_lock);
		timeout.tv_sec = 30;
		timeout.tv_usec = 0;
		switch (select(svc_maxfd+1, &readfds, NULL, NULL, &timeout)) {
		case -1:
			FD_ZERO(&readfds);
			if (errno == EINTR) {
				continue;
			}
			warn("svc_run: - select failed");
			return;
		case 0:
			__svc_clean_idle(&cleanfds, 30, FALSE);
			continue;
		default:
			svc_getreqset(&readfds);
		}
	}
}

#if defined(TIRPC_EPOLL)

/* static */ void
svc_run_epoll()
{
    int nfds;
    int timeout_s = 30;
    extern rwlock_t svc_fd_lock;

    if (! __svc_params->ev_u.epoll.events)
        __svc_params->ev_u.epoll.events =
            (struct epoll_event *) mem_alloc(
                __svc_params->ev_u.epoll.max_events * 
                sizeof(struct epoll_event));

    for (;;) {
        rwlock_rdlock(&svc_fd_lock);
        rwlock_unlock(&svc_fd_lock);
        switch (nfds = epoll_wait(
                    __svc_params->ev_u.epoll.epoll_fd,
                    __svc_params->ev_u.epoll.events, 
                    __svc_params->ev_u.epoll.max_events, 
                    timeout_s)) {
        case -1:
            if (errno == EINTR)
                continue;
            /* XXX epoll_ctl del all events ? */
            __pkg_params.warnx("svc_run: epoll_wait failed %d", nfds);
            return;
        case 0:
            __svc_clean_idle2(30, FALSE);
            continue;
        default:
            svc_getreqset_epoll(__svc_params->ev_u.epoll.events, nfds);
        } /* switch */
    } /* ;; */
}
#endif /* TIRPC_EPOLL */

void
svc_run()
{
    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        return (svc_run_epoll());
        break;
#endif
    default:
        return (svc_run_select());
        break;
    } /* switch */
}

/*
 *      This function causes svc_run() to exit by telling it that it has no
 *      more work to do.
 */
void
svc_exit()
{
    extern rwlock_t svc_fd_lock;

    rwlock_wrlock(&svc_fd_lock);
    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        /* XXXX need to actually signal svc_run */
        close(__svc_params->ev_u.epoll.epoll_fd);
        break;
#endif
    default:
	FD_ZERO(&svc_fdset);
        break;
    } /* switch */
    rwlock_unlock(&svc_fd_lock);
}
