/*-
 * Copyright (c) 1982, 1986, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *      @(#)time.h      8.5 (Berkeley) 5/4/95
 * $FreeBSD: head/sys/sys/time.h 224732 2011-08-09 14:06:50Z jonathan $
 */

#ifndef TIMESPEC_H

/* Operations on timespecs */
#define timespecclear(tvp)      ((tvp)->tv_sec = (tvp)->tv_nsec = 0)
#define timespecisset(tvp)      ((tvp)->tv_sec || (tvp)->tv_nsec)
#define timespeccmp(tvp, uvp, cmp)                                      \
    (((tvp)->tv_sec == (uvp)->tv_sec) ?                                 \
     ((tvp)->tv_nsec cmp (uvp)->tv_nsec) :                              \
     ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define timespecadd(vvp, uvp)                                           \
    do {                                                                \
        (vvp)->tv_sec += (uvp)->tv_sec;                                 \
        (vvp)->tv_nsec += (uvp)->tv_nsec;                               \
        if ((vvp)->tv_nsec >= 1000000000) {                             \
            (vvp)->tv_sec++;                                            \
            (vvp)->tv_nsec -= 1000000000;                               \
        }                                                               \
    } while (0)
#define timespec_adds(vvp, s)                                           \
    do {                                                                \
        (vvp)->tv_sec += s;                                             \
    } while (0)
#define timespec_addms(vvp, ms)                                         \
    do {                                                                \
        long as = (ms) / 1000;                                          \
        (vvp)->tv_sec += (ms) / 1000;                                   \
        (vvp)->tv_nsec += ((ms) - ((ms) % 1000) * 1000000);             \
        if ((vvp)->tv_nsec >= 1000000000) {                             \
            (vvp)->tv_sec++;                                            \
            (vvp)->tv_nsec -= 1000000000;                               \
        }                                                               \
    } while (0)
#define timespecsub(vvp, uvp)                                           \
    do {                                                                \
        (vvp)->tv_sec -= (uvp)->tv_sec;                                 \
        (vvp)->tv_nsec -= (uvp)->tv_nsec;                               \
        if ((vvp)->tv_nsec < 0) {                                       \
            (vvp)->tv_sec--;                                            \
            (vvp)->tv_nsec += 1000000000;                               \
        }                                                               \
    } while (0)
#endif /* TIMESPEC_H */
