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

#if !defined(_WIN32)
#include <sys/select.h>
#include <err.h>
#endif
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/types.h> /* before portable.h */
#include <misc/portable.h>
#if defined(TIRPC_EPOLL)
#include <misc/epoll.h> /* before rpc.h */
#endif
#include <rpc/rpc.h>
#include "rpc_com.h"

#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include <rpc/svc_rqst.h>

extern struct svc_params __svc_params[1];

bool __svc_clean_idle2(int timeout, bool cleanblock);

#if defined(TIRPC_EPOLL)
void svc_getreqset_epoll (struct epoll_event *events, int nfds);

/* static */ void
svc_run_epoll(void)
{
    /* TODO: rename */
    (void) svc_rqst_thrd_run(__svc_params->ev_u.evchan.id,
                             SVC_RQST_FLAG_NONE);
}
#endif /* TIRPC_EPOLL */

void
svc_run(void)
{
    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        svc_run_epoll();
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        __warnx(TIRPC_DEBUG_FLAG_SVC,
                "svc_run: unsupported event type");
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
    switch (__svc_params->ev_type) {
#if defined(TIRPC_EPOLL)
    case SVC_EVENT_EPOLL:
        /* signal shutdown backchannel */
        (void) svc_rqst_thrd_signal(__svc_params->ev_u.evchan.id,
                                    SVC_RQST_SIGNAL_SHUTDOWN);
        break;
#endif
    default:
        /* XXX formerly select/fd_set case, now placeholder for new
         * event systems, reworked select, etc. */
        break;
    } /* switch */
}
