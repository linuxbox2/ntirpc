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

#ifndef XDR_IOQ_H
#define XDR_IOQ_H

#include <stdint.h>
#include <stdbool.h>
#include <misc/queue.h>

struct v_rec
{
    TAILQ_ENTRY(v_rec) ioq;
    uint32_t refcnt;
    char *base;
    u_int off;
    u_int len;
    u_int size;
    u_int flags;
};

struct vpos_t
{
    struct v_rec *vrec;
    int32_t loff; /* logical byte offset  */
    int32_t bpos; /* buffer index (offset) in the current stream */
    int32_t boff; /* byte offset in buffer */
};

struct xdr_ioq
{
    XDR xdrs[1];
    TAILQ_ENTRY(xdr_ioq) ioq_s;
    struct {
        TAILQ_HEAD(vrq_tailq, v_rec) q;
        struct vpos_t fpos; /* fill position, GET|SETPOS */
        int size; /* count of buffer segments */
        uint32_t frag_len;
    } ioq;
    u_int def_bsize;
    u_int max_bsize;
    u_int flags;
    uint64_t id;
};

#define IOQ_FLAG_NONE          0x0000
#define IOQ_FLAG_RECLAIM       0x0001
#define IOQ_FLAG_BUFQ          0x0002
#define IOQ_FLAG_XTENDQ        0x0004
#define IOQ_FLAG_BALLOC        0x0008
#define IOQ_FLAG_REALLOC       0x0010

extern XDR *xdr_ioq_create(u_int def_bsize, u_int max_bsize, u_int flags);

#endif /* XDR_IOQ_H */
