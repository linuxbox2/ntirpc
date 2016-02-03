/*
 * Copyright (c) 2013 Linux Box Corporation.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include <intrinsic.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include <misc/opr.h>
#include "svc_ioq.h"


static inline void
cfconn_set_dead(SVCXPRT *xprt, struct x_vc_data *xd)
{
	mutex_lock(&xprt->xp_lock);
	xd->sx.strm_stat = XPRT_DIED;
	mutex_unlock(&xprt->xp_lock);
}

#define LAST_FRAG ((u_int32_t)(1 << 31))
#define MAXALLOCA (256)

static inline void
ioq_flushv(SVCXPRT *xprt, struct x_vc_data *xd, struct xdr_ioq *xioq)
{
	struct iovec *iov, *tiov, *wiov;
	struct poolq_entry *have;
	struct xdr_ioq_uv *data;
	ssize_t result;
	u_int32_t frag_header;
	u_int32_t fbytes;
	u_int32_t remaining = 0;
	u_int32_t vsize = (xioq->ioq_uv.uvqh.qcount + 1) * sizeof(struct iovec);
	int iw = 0;
	int ix = 1;

	if (unlikely(vsize > MAXALLOCA)) {
		iov = mem_alloc(vsize);
	} else {
		iov = alloca(vsize);
	}
	wiov = iov; /* position at initial fragment header */

	/* update the most recent data length, just in case */
	xdr_tail_update(xioq->xdrs);

	/* build list after initial fragment header (ix = 1 above) */
	TAILQ_FOREACH(have, &(xioq->ioq_uv.uvqh.qh), q) {
		data = IOQ_(have);
		tiov = iov + ix;
		tiov->iov_base = data->v.vio_head;
		tiov->iov_len = ioquv_length(data);
		remaining += tiov->iov_len;
		ix++;
	}

	while (remaining > 0) {
		if (iw == 0) {
			/* new fragment header, determine last iov */
			fbytes = 0;
			for (tiov = &wiov[++iw];
			     (tiov < &iov[ix]) && (iw < __svc_maxiov);
			     ++tiov, ++iw) {
				fbytes += tiov->iov_len;

				/* check for fragment value overflow */
				/* never happens, see ganesha FSAL_MAXIOSIZE */
				if (unlikely(fbytes >= LAST_FRAG)) {
					fbytes -= tiov->iov_len;
					break;
				}
			} /* for */

			/* fragment length doesn't include fragment header */
			if (&wiov[iw] < &iov[ix]) {
				frag_header = htonl((u_int32_t) (fbytes));
			} else {
				frag_header = htonl((u_int32_t) (fbytes | LAST_FRAG));
			}
			wiov->iov_base = &(frag_header);
			wiov->iov_len = sizeof(u_int32_t);

			/* writev return includes fragment header */
			remaining += sizeof(u_int32_t);
			fbytes += sizeof(u_int32_t);
		}

		/* blocking write */
		result = writev(xprt->xp_fd, wiov, iw);
		remaining -= result;

		if (result == fbytes) {
			wiov += iw - 1;
			iw = 0;
			continue;
		}
		if (unlikely(result < 0)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() writev failed (%d)\n",
				__func__, errno);
			cfconn_set_dead(xprt, xd);
			break;
		}
		fbytes -= result;

		/* rare? writev underrun? (assume never overrun) */
		for (tiov = wiov; iw > 0; ++tiov, --iw) {
			if (tiov->iov_len > result) {
				tiov->iov_len -= result;
				tiov->iov_base += result;
				wiov = tiov;
				break;
			} else {
				result -= tiov->iov_len;
			}
		} /* for */
	} /* while */

	if (unlikely(vsize > MAXALLOCA)) {
		mem_free(iov, vsize);
	}
}

static void
svc_ioq_callback(struct work_pool_entry *wpe)
{
	struct x_vc_data *xd = (struct x_vc_data *)wpe;
	SVCXPRT *xprt = (SVCXPRT *)wpe->arg;
	struct poolq_entry *have;
	struct xdr_ioq *xioq;

	/* qmutex more fine grained than xp_lock */
	for (;;) {
		mutex_lock(&xd->shared.ioq.qmutex);
		have = TAILQ_FIRST(&xd->shared.ioq.qh);
		if (unlikely(!have)) {
			xd->shared.active = false;
			mutex_unlock(&xd->shared.ioq.qmutex);
			SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			return;
		}

		TAILQ_REMOVE(&xd->shared.ioq.qh, have, q);
		(xd->shared.ioq.qcount)--;
		/* do i/o unlocked */
		mutex_unlock(&xd->shared.ioq.qmutex);
		xioq = _IOQ(have);

		if (svc_work_pool.params.thrd_max
		 && !(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
			/* all systems are go! */
			ioq_flushv(xprt, xd, xioq);
		}
		XDR_DESTROY(xioq->xdrs);
	}

	return;
}

void
svc_ioq_append(SVCXPRT *xprt, struct x_vc_data *xd, XDR *xdrs)
{
	if (unlikely(!svc_work_pool.params.thrd_max
		  || (xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED))) {
		/* discard */
		XDR_DESTROY(xdrs);
		return;
	}

	/* qmutex more fine grained than xp_lock */
	mutex_lock(&xd->shared.ioq.qmutex);
	(xd->shared.ioq.qcount)++;
	TAILQ_INSERT_TAIL(&xd->shared.ioq.qh, &(XIOQ(xdrs)->ioq_s), q);

	if (!xd->shared.active) {
		xd->shared.active = true;
		xd->wpe.fun = svc_ioq_callback;
		xd->wpe.arg = xprt;
		mutex_unlock(&xd->shared.ioq.qmutex);
		SVC_REF(xprt, SVC_REF_FLAG_NONE);
		work_pool_submit(&svc_work_pool, &xd->wpe);
	} else {
		/* queuing multiple output requests for worker efficiency */
		mutex_unlock(&xd->shared.ioq.qmutex);
	}
}
