/*
 * Copyright (c) 2009,s Sun Microsystems, Inc.
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

#include <config.h>
#include <sys/cdefs.h>
#include <sys/cdefs.h>

/*
 * xdr_rec.c, Implements TCP/IP based XDR streams with a "record marking"
 * layer above tcp (for rpc's use).
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * These routines interface XDRSTREAMS to a tcp/ip connection.
 * There is a record marking layer between the xdr stream
 * and the tcp transport level.  A record is composed on one or more
 * record fragments.  A record fragment is a thirty-two bit header followed
 * by n bytes of data, where n is contained in the header.  The header
 * is represented as a htonl(u_long).  Thegh order bit encodes
 * whether or not the fragment is the last fragment of the record
 * (1 => fragment is last, 0 => more fragments to follow.
 * The other 31 bits encode the byte length of the fragment.
 */

#include <sys/types.h>

#if !defined(_WIN32)
#include <netinet/in.h>
#include <err.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include "rpc_com.h"
#include <misc/city.h>
#include <rpc/rpc_cksum.h>
#include <intrinsic.h>

static bool xdr_inrec_getlong(XDR *, long *);
static bool xdr_inrec_putlong(XDR *, const long *);
static bool xdr_inrec_getbytes(XDR *, char *, u_int);

static bool xdr_inrec_putbytes(XDR *, const char *, u_int);
static u_int xdr_inrec_getpos(XDR *);
static bool xdr_inrec_setpos(XDR *, u_int);
static int32_t *xdr_inrec_inline(XDR *, u_int);
static void xdr_inrec_destroy(XDR *);
static bool xdr_inrec_noop(void);

extern bool xdr_inrec_readahead(XDR *, u_int);

typedef bool (* dummyfunc3)(XDR *, int, void *);
typedef bool (* dummy_getbufs)(XDR *, xdr_uio *, u_int);
typedef bool (* dummy_putbufs)(XDR *, xdr_uio *, u_int);

static const struct  xdr_ops xdr_inrec_ops = {
    xdr_inrec_getlong,
    xdr_inrec_putlong,
    xdr_inrec_getbytes,
    xdr_inrec_putbytes,
    xdr_inrec_getpos,
    xdr_inrec_setpos,
    xdr_inrec_inline,
    xdr_inrec_destroy,
    (dummyfunc3) xdr_inrec_noop, /* x_control */
    (dummy_getbufs) xdr_inrec_noop, /* x_getbufs */
    (dummy_putbufs) xdr_inrec_noop  /* x_putbufs */
};

/*
 * A record is composed of one or more record fragments.
 * A record fragment is a four-byte header followed by zero to
 * 2**32-1 bytes.  The header is treated as a long unsigned and is
 * encode/decoded to the network via htonl/ntohl.  The low order 31 bits
 * are a byte count of the fragment.  The highest order bit is a boolean:
 * 1 => this fragment is the last fragment of the record,
 * 0 => this fragment is followed by more fragment(s).
 *
 * The fragment/record machinery is not general;  it is constructed to
 * meet the needs of xdr and rpc based on tcp.
 */

#define LAST_FRAG ((u_int32_t)(1 << 31))

typedef struct rec_strm {
    XDR *xdrs;
    char *tcp_handle;
    /*
     * in-coming bits
     */
    int (*readit)(XDR *, void *, void *, int);
    u_int32_t in_size; /* fixed size of the input buffer */
    char *in_base;
    char *in_finger; /* location of next byte to be had */
    char *in_boundry; /* can read up to this location */
    int32_t fbtbc;  /* fragment bytes to be consumed */
    int32_t offset;
    bool last_frag;
    u_int recvsize;
    uint64_t cksum;
    uint32_t cklen;
    bool in_haveheader;
    u_int32_t in_header;
    int in_maxrec;
} RECSTREAM;

static u_int fix_buf_size(u_int);
static bool fill_input_buf(RECSTREAM *, int32_t);
static bool get_input_bytes(RECSTREAM *, char *, int32_t, int32_t);
static bool set_input_fragment(RECSTREAM *, int32_t);
static bool skip_input_bytes(RECSTREAM *, long);
static void compute_buffer_cksum(RECSTREAM *rstrm);

/*
 * Create an xdr handle for xdrrec
 * xdr_inrec_create fills in xdrs.  Sendsize and recvsize are
 * send and recv buffer sizes (0 => use default).
 * tcp_handle is an opaque handle that is passed as the first parameter to
 * the procedures readit and writeit.  Readit and writeit are read and
 * write respectively.   They are like the system
 * calls expect that they take an opaque handle rather than an fd.
 */
void
xdr_inrec_create(XDR *xdrs,
              u_int recvsize,
              void *tcp_handle,
              /* like read, but pass it a tcp_handle, not sock */
              int (*readit)(XDR *, void *, void *, int))
{
    RECSTREAM *rstrm = mem_alloc(sizeof(RECSTREAM));

    if (rstrm == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_inrec_create: out of memory");
        /*
         *  This is bad.  Should rework xdr_inrec_create to
         *  return a handle, and in this case return NULL
         */
        return;
    }
    rstrm->recvsize = recvsize = fix_buf_size(recvsize);
    rstrm->in_base = mem_alloc(recvsize);
    if (rstrm->in_base == NULL) {
        __warnx(TIRPC_DEBUG_FLAG_XDRREC,
                "xdr_inrec_create: out of memory");
        mem_free(rstrm, sizeof(RECSTREAM));
        return;
    }
    /*
     * now the rest ...
     */
    xdrs->x_ops = &xdr_inrec_ops;
    xdrs->x_lib[0] = NULL;
    xdrs->x_lib[1] = NULL;
    xdrs->x_public = NULL;
    xdrs->x_private = rstrm;
    xdrs->x_flags = XDR_FLAG_CKSUM;
    rstrm->xdrs = xdrs;
    rstrm->tcp_handle = tcp_handle;
    rstrm->readit = readit;
    rstrm->in_size = recvsize;
    rstrm->in_boundry = rstrm->in_base;
    rstrm->in_finger = (rstrm->in_boundry += recvsize);
    rstrm->fbtbc = 0;
    rstrm->last_frag = TRUE;
    rstrm->in_haveheader = FALSE;
    rstrm->offset = 0;
    rstrm->cksum = 0;
    rstrm->cklen = 256;
}

/* Compute 64-bit checksum of the first cnt bytes (or offset, whichever is
 * less) in the receive buffer.  Use only as directed.
 */
uint64_t
xdr_inrec_cksum(XDR *xdrs)
{
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);

    /* handle checksumming if requested (short request case) */
    if (xdrs->x_flags & XDR_FLAG_CKSUM) {
        if (! (rstrm->cksum)) {
            if (rstrm->cklen) {
                compute_buffer_cksum(rstrm);
            }
        }
    }

    return (rstrm->cksum);
}

/*
 * The routines defined below are the xdr ops which will go into the
 * xdr handle filled in by xdr_inrec_create.
 */
static bool
xdr_inrec_getlong(XDR *xdrs,  long *lp)
{
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int32_t *buflp = (int32_t *)(void *)(rstrm->in_finger);
    int32_t mylong;

    /* first try the inline, fast case */
    if ((rstrm->fbtbc >= sizeof(int32_t)) &&
        ((PtrToUlong(rstrm->in_boundry) - PtrToUlong(buflp)) >=
	 sizeof(int32_t))) {
        *lp = (long)ntohl((u_int32_t)(*buflp));
        rstrm->fbtbc -= sizeof(int32_t);
        rstrm->in_finger += sizeof(int32_t);
    } else {
        if (! xdr_inrec_getbytes(xdrs, (char *)(void *)&mylong,
                              sizeof(int32_t)))
            return (FALSE);
        *lp = (long)ntohl((u_int32_t)mylong);
    }
    return (TRUE);
}

static bool
xdr_inrec_putlong(XDR *xdrs, const long *lp)
{
    return (FALSE);
}

static bool  /* must manage buffers, fragments, and records */
xdr_inrec_getbytes(XDR *xdrs, char *addr, u_int len)
{
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);
    int current;

    while (len > 0) {
        current = (int)rstrm->fbtbc;
        if (current == 0) {
            if (rstrm->last_frag)
                return (FALSE);
            if (! set_input_fragment(rstrm, INT_MAX))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        if (! get_input_bytes(rstrm, addr, current, INT_MAX))
            return (FALSE);
        addr += current;
        rstrm->fbtbc -= current;
        len -= current;
        /* handle checksumming if requested */
        if (xdrs->x_flags & XDR_FLAG_CKSUM) {
            if (rstrm->cklen) {
                if (! (rstrm->cksum)) {
                    if (rstrm->offset >= rstrm->cklen) {
                        compute_buffer_cksum(rstrm);
                    }
                }
            }
        }
    }
    return (TRUE);
}

static bool
xdr_inrec_putbytes(XDR *xdrs, const char *addr, u_int len)
{
    return (FALSE);
}

static u_int
xdr_inrec_getpos(XDR *xdrs)
{
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    off_t pos;

    switch (xdrs->x_op) {

    case XDR_DECODE:
        pos = rstrm->in_boundry - rstrm->in_finger
            - BYTES_PER_XDR_UNIT;
        break;

    default:
        pos = (off_t) -1;
        break;
    }
    return ((u_int) pos);
}

static bool
xdr_inrec_setpos(XDR *xdrs, u_int pos)
{
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    u_int currpos = xdr_inrec_getpos(xdrs);
    int delta = currpos - pos;
    char *newpos;

    if ((int)currpos != -1)
        switch (xdrs->x_op) {

        case XDR_DECODE:
            newpos = rstrm->in_finger - delta;
            if ((delta < (int)(rstrm->fbtbc)) &&
                (newpos <= rstrm->in_boundry) &&
                (newpos >= rstrm->in_base)) {
                rstrm->in_finger = newpos;
                rstrm->fbtbc -= delta;
                return (TRUE);
            }
            break;

        case XDR_ENCODE:
        case XDR_FREE:
            break;
        }
    return (FALSE);
}

bool
xdr_inrec_readahead(XDR *xdrs, u_int maxfraglen)
{
    RECSTREAM *rstrm;
    int current;

    rstrm = (RECSTREAM *)xdrs->x_private;

    current = (int)rstrm->fbtbc;
    if (current == 0) {
        if (rstrm->last_frag) {
            return (FALSE);
        }
        if (! set_input_fragment(rstrm, maxfraglen))
            return (FALSE);
    }
    return (TRUE);
}

static int32_t *
xdr_inrec_inline(XDR *xdrs, u_int len)
{
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;
    int32_t *buf = NULL;

    switch (xdrs->x_op) {

    case XDR_DECODE:
        if ((len <= rstrm->fbtbc) &&
            ((rstrm->in_finger + len) <= rstrm->in_boundry)) {
            buf = (int32_t *)(void *)rstrm->in_finger;
            rstrm->fbtbc -= len;
            rstrm->in_finger += len;
        }
        break;

    case XDR_ENCODE:
    case XDR_FREE:
        break;
    }
    return (buf);
}

static void
xdr_inrec_destroy(XDR *xdrs)
{
    RECSTREAM *rstrm = (RECSTREAM *)xdrs->x_private;

    mem_free(rstrm->in_base, rstrm->recvsize);
    mem_free(rstrm, sizeof(RECSTREAM));
}


/*
 * Exported routines to manage xdr records
 */

/*
 * Before reading (deserializing from the stream), one should always call
 * this procedure to guarantee proper record alignment.
 */
bool
xdr_inrec_skiprecord(XDR *xdrs)
{
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);

    while (rstrm->fbtbc > 0 || (! rstrm->last_frag)) {
        if (! skip_input_bytes(rstrm, rstrm->fbtbc))
            return (FALSE);
        rstrm->fbtbc = 0;
        if ((! rstrm->last_frag) && (! set_input_fragment(rstrm, INT_MAX)))
            return (FALSE);
    }
    rstrm->last_frag = FALSE;
    rstrm->offset = 0;
    rstrm->cksum = 0;
    return (TRUE);
}

/*
 * Look ahead function.
 * Returns TRUE iff there is no more input in the buffer
 * after consuming the rest of the current record.
 */
bool
xdr_inrec_eof(XDR *xdrs)
{
    RECSTREAM *rstrm = (RECSTREAM *)(xdrs->x_private);

    while (rstrm->fbtbc > 0 || (! rstrm->last_frag)) {
        if (! skip_input_bytes(rstrm, rstrm->fbtbc))
            return (TRUE);
        rstrm->fbtbc = 0;
        if ((! rstrm->last_frag) && (! set_input_fragment(rstrm, INT_MAX)))
            return (TRUE);
    }
    if (rstrm->in_finger == rstrm->in_boundry)
        return (TRUE);
    return (FALSE);
}

static bool  /* knows nothing about records!  Only about input buffers */
fill_input_buf(RECSTREAM *rstrm, int32_t maxreadahead)
{
    char *where;
    u_int32_t i;
    int len;

    where = rstrm->in_base;
    i = (u_int32_t)(PtrToUlong(rstrm->in_boundry) % BYTES_PER_XDR_UNIT);
    where += i;
    len = MIN(((u_int32_t)(rstrm->in_size - i)), maxreadahead);
    if ((len = (*(rstrm->readit))(rstrm->xdrs, rstrm->tcp_handle, where,
                                  len)) == -1)
        return (FALSE);
    rstrm->in_finger = where;
    where += len;
    rstrm->in_boundry = where;
    /* cksum lookahead */
    rstrm->offset += len;
    return (TRUE);
}

static bool  /* knows nothing about records!  Only about input buffers */
get_input_bytes(RECSTREAM *rstrm, char *addr, int32_t len, int32_t maxreadahead)
{
    int32_t current;

    while (len > 0) {
      current = (PtrToUlong(rstrm->in_boundry) -
                 PtrToUlong(rstrm->in_finger));
        if (current == 0) {
            if (! fill_input_buf(rstrm, maxreadahead))
                return (FALSE);
            continue;
        }
        current = (len < current) ? len : current;
        memmove(addr, rstrm->in_finger, current);
        rstrm->in_finger += current;
        addr += current;
        len -= current;
    }
    return (TRUE);
}

static bool  /* next two bytes of the input stream are treated as a header */
set_input_fragment(RECSTREAM *rstrm, int32_t maxreadahead)
{
    u_int32_t header;

    if (! get_input_bytes(rstrm, (char *)(void *)&header, sizeof(header),
            maxreadahead))
        return (FALSE);
    header = ntohl(header);
    rstrm->last_frag = ((header & LAST_FRAG) == 0) ? FALSE : TRUE;
    /*
     * Sanity check. Try not to accept wildly incorrect
     * record sizes. Unfortunately, the only record size
     * we can positively identify as being 'wildly incorrect'
     * is zero. Ridiculously large record sizes may look wrong,
     * but we don't have any way to be certain that they aren't
     * what the client actually intended to send us.
     */
    if (header == 0)
        return(FALSE);
    rstrm->fbtbc = header & (~LAST_FRAG);
    return (TRUE);
}

static bool  /* consumes input bytes; knows nothing about records! */
skip_input_bytes(RECSTREAM *rstrm, long cnt)
{
    u_int32_t current;

    while (cnt > 0) {
      current = (size_t)(PtrToUlong(rstrm->in_boundry) -
			 PtrToUlong(rstrm->in_finger));
        if (current == 0) {
            if (! fill_input_buf(rstrm, INT_MAX))
                return (FALSE);
            continue;
        }
        current = (u_int32_t)((cnt < current) ? cnt : current);
        rstrm->in_finger += current;
        cnt -= current;
    }
    return (TRUE);
}

static u_int
fix_buf_size(u_int s)
{

    if (s < 100)
        s = 4000;
    return (RNDUP(s));
}

static void
compute_buffer_cksum(RECSTREAM *rstrm)
{
#if 1
    /* CithHash64 is -substantially- faster than crc32c from FreeBSD
     * SCTP, so prefer it until fast crc32c bests it */
    rstrm->cksum =
        CityHash64WithSeed(rstrm->in_base,
                           MIN(rstrm->cklen, rstrm->offset),
                           103);
#else
    rstrm->cksum = calculate_crc32c(0, rstrm->in_base,
                                    MIN(rstrm->cklen, rstrm->offset));
#endif
}

static bool
xdr_inrec_noop(void)
{
    return (FALSE);
}
