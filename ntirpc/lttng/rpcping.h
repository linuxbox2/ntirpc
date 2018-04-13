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


#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER rpcping

#if !defined(GANESHA_LTTNG_RPCPING_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define GANESHA_LTTNG_RPCPING_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
	rpcping,
	test,
	TP_ARGS(const char *, file,
		const char *, function,
		unsigned int, line,
		char *, message),
	TP_FIELDS(
		ctf_string(file, file)
		ctf_string(fnc, function)
		ctf_integer(unsigned int, line, line)
		ctf_string(msg, message)
	)
)

TRACEPOINT_LOGLEVEL(
	rpcping,
	test,
	TRACE_INFO)

#endif /* GANESHA_LTTNG_RPCPING_TP_H */

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "lttng/rpcping.h"

#include <lttng/tracepoint-event.h>
