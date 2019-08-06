/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2018 Red Hat, Inc. and/or its affiliates.
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

#include "config.h"
#include <stdint.h>

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER xprt

#if !defined(GANESHA_LTTNG_XPRT_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define GANESHA_LTTNG_XPRT_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
	xprt,
	ref,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, count),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(uint32_t, count, count)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	ref,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	unref,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, count),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(uint32_t, count, count)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	unref,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	destroy,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint16_t, flags),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(uint16_t, count, flags)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	destroy,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	unhook,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, flags),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(uint32_t, flags, flags)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	unhook,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	rearm,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, flags),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(uint32_t, flags, flags)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	rearm,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	hook,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, flags),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(uint32_t, flags, flags)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	hook,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	event,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		uint32_t, xp_flags,
		uint32_t, ev_flag),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(uint32_t, xp_flags, xp_flags)
		ctf_integer_hex(uint32_t, ev_flag, ev_flag)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	event,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	recv,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		unsigned int, destroyed,
		unsigned int, count),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(unsigned int, destroyed, destroyed)
		ctf_integer(unsigned int, count, count)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	recv,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	send,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		unsigned int, destroyed,
		unsigned int, count),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(unsigned int, destroyed, destroyed)
		ctf_integer(unsigned int, count, count)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	send,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	write_blocked,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	write_blocked,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	write_complete,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		unsigned int, has_blocked),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(unsigned int, has_blocked, has_blocked)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	write_complete,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	sendmsg,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		unsigned int, remaining,
		unsigned int, frag_needed,
		unsigned int, iov_count),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer(unsigned int, remaining, remaining)
		ctf_integer(unsigned int, frag_needed, frag_needed)
		ctf_integer(unsigned int, iov_count, iov_count)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	sendmsg,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	mutex,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	mutex,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	funcin,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	funcin,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	recv_frag,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		int32_t, frag_len),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(int32_t, frag_len, frag_len)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	recv_frag,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	recv_bytes,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		int32_t, frag_remain,
		ssize_t, bytes),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_integer_hex(int32_t, frag_remain, frag_remain)
		ctf_integer_hex(ssize_t, bytes, bytes)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	recv_bytes,
	TRACE_INFO)

TRACEPOINT_EVENT(
	xprt,
	recv_exit,
	TP_ARGS(const char *, function,
		unsigned int, line,
		void *, xprt,
		const char *, reason,
		int, code),
	TP_FIELDS(
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_integer_hex(void *, xprt, xprt)
		ctf_string(reason, reason)
		ctf_integer(int, code, code)
	)
)

TRACEPOINT_LOGLEVEL(
	xprt,
	recv_exit,
	TRACE_INFO)

#endif /* GANESHA_LTTNG_XPRT_TP_H */

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "lttng/xprt.h"

#include <lttng/tracepoint-event.h>
