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
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include <misc/opr.h>
#include "svc_ioq.h"

/* Queue per interface, determined per transport socket.
 *
 * Ideally, these would be some variant of weighted fair queuing.  Currently,
 * assuming supplied by underlying OS.
 *
 * The assigned thread should have affinity for the interface.  Therefore, the
 * first thread arriving for each interface is used for all subsequent work,
 * until the interface is idle.  This assumes that the output interface is
 * closely associated with the input interface.
 *
 * Note that this is a static fixed size list of interfaces.  In most cases,
 * many of these entries will be unused.
 *
 * For efficiency, a mask is applied to the ifindex, possibly causing overlap of
 * multiple interfaces.  The size is selected to be larger than expected number
 * of concurrently active interfaces.  Size must be a power of 2 for mask.
 */
#define IOQ_IF_SIZE (16)
#define IOQ_IF_MASK (IOQ_IF_SIZE - 1)
struct poolq_head ioq_ifqh[IOQ_IF_SIZE];

void
svc_ioq_init(void)
{
	struct poolq_head *ifph = &ioq_ifqh[0];
	int i = 0;

	for (; i < IOQ_IF_SIZE; ifph++, i++) {
		ifph->qcount = 0;
		TAILQ_INIT(&ifph->qh);
		mutex_init(&ifph->qmutex, NULL);
	}
}

#define LAST_FRAG ((u_int32_t)(1 << 31))
#define MAXALLOCA (256)

static inline void
svc_ioq_flushv(SVCXPRT *xprt, struct xdr_ioq *xioq)
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
			SVC_DESTROY(xprt);
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
svc_ioq_write(SVCXPRT *xprt, struct xdr_ioq *xioq, struct poolq_head *ifph)
{
	struct poolq_entry *have;

	for (;;) {
		/* do i/o unlocked */
		if (svc_work_pool.params.thrd_max
		 && !(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
			/* all systems are go! */
			svc_ioq_flushv(xprt, xioq);
		}
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		XDR_DESTROY(xioq->xdrs);

		mutex_lock(&ifph->qmutex);
		if (--(ifph->qcount) == 0)
			break;

		have = TAILQ_FIRST(&ifph->qh);
		TAILQ_REMOVE(&ifph->qh, have, q);
		mutex_unlock(&ifph->qmutex);

		xioq = _IOQ(have);
		xprt = (SVCXPRT *)xioq->xdrs[0].x_lib[1];
	}
	mutex_unlock(&ifph->qmutex);
}

static void
svc_ioq_write_callback(struct work_pool_entry *wpe)
{
	struct xdr_ioq *xioq = opr_containerof(wpe, struct xdr_ioq, ioq_wpe);
	SVCXPRT *xprt = (SVCXPRT *)xioq->xdrs[0].x_lib[1];
	struct poolq_head *ifph = &ioq_ifqh[xprt->xp_ifindex & IOQ_IF_MASK];

	svc_ioq_write(xprt, xioq, ifph);
}

void
svc_ioq_write_now(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct poolq_head *ifph = &ioq_ifqh[xprt->xp_ifindex & IOQ_IF_MASK];

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	mutex_lock(&ifph->qmutex);

	if ((ifph->qcount)++ > 0) {
		/* queue additional output requests without task switch */
		TAILQ_INSERT_TAIL(&ifph->qh, &(xioq->ioq_s), q);
		mutex_unlock(&ifph->qmutex);
		return;
	}
	mutex_unlock(&ifph->qmutex);

	/* handle this output request without queuing, then any additional
	 * output requests without a task switch (using this thread).
	 */
	svc_ioq_write(xprt, xioq, ifph);
}

/*
 * Handle rare case of first output followed by heavy traffic that prevents the
 * original thread from continuing for too long.
 *
 * In the more common case, server traffic will already have begun and this
 * will rapidly queue the output and return.
 */
void
svc_ioq_write_submit(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct poolq_head *ifph = &ioq_ifqh[xprt->xp_ifindex & IOQ_IF_MASK];

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	mutex_lock(&ifph->qmutex);

	if ((ifph->qcount)++ > 0) {
		/* queue additional output requests, they will be handled by
		 * existing thread without another task switch.
		 */
		TAILQ_INSERT_TAIL(&ifph->qh, &(xioq->ioq_s), q);
		mutex_unlock(&ifph->qmutex);
		return;
	}
	mutex_unlock(&ifph->qmutex);

	xioq->ioq_wpe.fun = svc_ioq_write_callback;
	work_pool_submit(&svc_work_pool, &xioq->ioq_wpe);
}
