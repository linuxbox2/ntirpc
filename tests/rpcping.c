/*
 * Copyright (c) 2017 DESY Hamburg DMG-Division.
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * AUTHOR: Tigran Mkrtchayn (tigran.mkrtchyan@desy.de)
 * AUTHOR: William Allen Simpson <wsimpson@redhat.com>
 *
 * This code is released into the "public domain" by its author(s). 
 * Anybody may use, alter, and distribute the code without restriction. 
 * The author(s) make no guarantees, and take no liability of any kind
 * for use of this code.
 */

/**
 * @file rpcping.c
 * @author William Allen Simpson <wsimpson@redhat.com>
 * @brief RPC ping
 *
 * @section DESCRIPTION
 *
 * Simple RPC ping test.
 *
 * @note    Partially based upon previous work:
 *          2012 April 22 tigran.mkrtchyan
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/times.h>
#include <pthread.h>
#include <rpc/rpc.h>
#include <rpc/svc_auth.h>

static pthread_mutex_t rpcpring_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rpcpring_cond = PTHREAD_COND_INITIALIZER;
static volatile unsigned rpcpring_threads;

static struct timespec to = {30, 0};

struct state {
	CLIENT *handle;
	double averageTime;
	clock_t rtime;
	int count;
	int proc;
	int requests;
	int id;
};

static void
worker_cb(struct clnt_req *cc)
{
	CLIENT *clnt = cc->cc_clnt;
	struct state *s = clnt->cl_u1;
	struct tms dumm;

	clnt_req_release(cc);
	if (--s->requests > 0) {
		return;
	}
	s->averageTime = s->count
			 / ((double) (times(&dumm) - s->rtime)
			 / (double) sysconf(_SC_CLK_TCK));

	CLNT_DESTROY(clnt);
	pthread_mutex_lock(&rpcpring_mutex);
	--rpcpring_threads;
	pthread_mutex_unlock(&rpcpring_mutex);
	pthread_cond_broadcast(&rpcpring_cond);
}

static void *
worker(void *arg)
{
	struct state *s = arg;
	struct clnt_req *cc;
	struct tms dumm;
	int i;

	s->rtime = times(&dumm);
	for (i = 0; i < s->count; i++) {
		cc = calloc(1, sizeof(*cc));
		clnt_req_fill(cc, s->handle, authnone_ncreate(), s->proc,
			      (xdrproc_t) xdr_void, NULL,
			      (xdrproc_t) xdr_void, NULL);

		s->requests++;
		if (clnt_req_setup(cc, to) != RPC_SUCCESS) {
			rpc_perror(&cc->cc_error, "clnt_req_setup failed");
			s->count = s->requests;
			worker_cb(cc);
			break;
		}
		cc->cc_refreshes = 1;
		cc->cc_process_cb = worker_cb;

		cc->cc_error.re_status = CLNT_CALL_BACK(cc);
		if (cc->cc_error.re_status != RPC_SUCCESS) {
			rpc_perror(&cc->cc_error, "CLNT_CALL_BACK failed");
			s->count = s->requests;
			worker_cb(cc);
			break;
		}
	}

	return NULL;
}

static enum xprt_stat
decode_request(SVCXPRT *xprt, XDR *xdrs)
{
	struct svc_req *req = calloc(1, sizeof(*req));
	enum xprt_stat stat;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	req->rq_xprt = xprt;
	req->rq_xdrs = xdrs;
	req->rq_refs = 1;

	stat = SVC_DECODE(req);

	if (req->rq_auth)
		SVCAUTH_RELEASE(req);

	XDR_DESTROY(req->rq_xdrs);
	SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
	free(req);
	return stat;
}

int main(int argc, char *argv[])
{
	svc_init_params svc_params;
	CLIENT *clnt;
	struct state *s;
	struct state *states;
	double total;
	int i;
	int nthreads;
	int count = 1500; /* observed optimal concurrent requests */
	int prog = 100003; /* nfs */
	int vers = 3; /* allow raw, rdma, tcp, udp by default */
	int proc = 0;

	if (argc < 4 || argc > 8) {
		printf("Usage: rpcping <protocol> <host> <nthreads> [<count> [<program> [<version> [<procedure>]]]]\n");
		exit(1);
	}

	nthreads = atoi(argv[3]);

	if (argc > 4) {
		count = atoi(argv[4]);
	}

	if (argc > 5) {
		prog = atoi(argv[5]);
	}

	if (argc > 6) {
		vers = atoi(argv[6]);
	}

	if (argc > 7) {
		proc = atoi(argv[7]);
	}

	states = calloc(nthreads, sizeof(struct state));
	if (!states) {
		perror("calloc failed");
		exit(1);
	}

	memset(&svc_params, 0, sizeof(svc_params));
	svc_params.request_cb = decode_request;
	svc_params.flags = SVC_INIT_EPOLL | SVC_INIT_NOREG_XPRTS;
	svc_params.max_events = 512;

	if (!svc_init(&svc_params)) {
		perror("svc_init failed");
		exit(1);
	}

	for (i = 0; i < nthreads; i++) {
		pthread_t t;

		clnt = clnt_ncreate(argv[2], prog, vers, argv[1]);
		if (CLNT_FAILURE(clnt)) {
			rpc_perror(&clnt->cl_error, "clnt_ncreate failed");
			exit(2);
		}
		s = &states[i];
		clnt->cl_u1 = s;

		s->handle = clnt;
		s->id = i;
		s->count = count;
		s->proc = proc;
		pthread_create(&t, NULL, worker, s);
		rpcpring_threads++;
	}

	while (rpcpring_threads) {
		pthread_cond_wait(&rpcpring_cond, &rpcpring_mutex);
	}

	total = 0.0;
	for (i = 0; i < nthreads; i++) {
		s = &states[i];
		total += s->averageTime;
	}

	fprintf(stdout, "rpcping %s %s threads=%d count=%d (program=%d version=%d procedure=%d): %2.4lf, total %2.4lf\n",
		argv[1], argv[2], nthreads, count, prog, vers, proc,
		total / nthreads, total);
	fflush(stdout);
	return (0);
}
