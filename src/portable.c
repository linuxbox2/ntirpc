/*
 * Copyright (c) 2012 Linux Box Corporation.
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

#include <config.h>
#include <misc/portable.h>
#include <misc/stdio.h>
#include <stdbool.h>
#include <reentrant.h>

#ifdef __APPLE__
int
clock_gettime(clockid_t clock, struct timespec *ts)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  ts->tv_sec = tv.tv_sec;
  ts->tv_nsec = tv.tv_usec * 1000UL;
  return 0;
}
#endif

#if defined(_WIN32)
pthread_mutex_t clock_mtx = PTHREAD_MUTEX_INITIALIZER;

int
clock_gettime(clockid_t clock, struct timespec *ts)
{
  static bool initialized;
  static LARGE_INTEGER start, freq, ctr, m;
  static double ftom;
  bool rslt = false;

  if (! initialized) {
      mutex_lock(&clock_mtx);
      if (! initialized) {
	QueryPerformanceCounter(&start);
	(void) QueryPerformanceFrequency(&freq); /* XXXX can be 0, would need to fall back */
	ftom = (double) freq.QuadPart / 1000000.;
      }      
      mutex_unlock(&clock_mtx);
  }

  rslt = QueryPerformanceCounter(&ctr);
  switch (rslt) {
  case 0:
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
    break;
  default:
    ctr.QuadPart -= start.QuadPart;
    m.QuadPart = ctr.QuadPart / ftom;
    ctr.QuadPart =  m.QuadPart % 1000000;
    ts->tv_sec = m.QuadPart / 1000000;
    ts->tv_nsec = ctr.QuadPart * 1000;
  }

  return (rslt);
}

/* XXX this mutex actually -is- serializing calls, however, these
 * calls should be rare */
pthread_mutex_t warn_mtx = PTHREAD_MUTEX_INITIALIZER;

void warnx(const char *fmt, ...)
{
	va_list ap;
	static char msg[128];

	va_start(ap, fmt);
	if (fmt != NULL)
		_vsnprintf(msg, 128, fmt, ap);
	else
		msg[0]='\0';
	va_end(ap);
	fprintf(stderr, "%s\n", msg);
}

#endif /* _WIN32 */
