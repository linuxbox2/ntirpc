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
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>
#include <getopt.h>
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

static int
get_conn_fd(const char *host, int hbport)
{
	struct addrinfo *res, *fr;
	int r, fd = 0;

	r = getaddrinfo(host, NULL, NULL, &res);
	if (r) {
		return 0;
	}
	fr = res;

	while (res) {
		fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd <= 0)
			goto next;
		switch (res->ai_family) {
		case AF_INET:
		{
			struct sockaddr_in *sin;
			sin = (struct sockaddr_in *) res->ai_addr;
			sin->sin_port = htons(hbport);
			r = connect(fd, (struct sockaddr *) sin,
				    sizeof(struct sockaddr));
			if (!!r) {
				close(fd);
			} else
				goto done;
		}
			break;
		case AF_INET6:
		{
			struct sockaddr_in6 *sin6;
			sin6 = (struct sockaddr_in6 *) res->ai_addr;
			sin6->sin6_port = htons(hbport);
			r = connect(fd, (struct sockaddr *) sin6,
				    sizeof(struct sockaddr));
			if (!!r) {
				close(fd);
			} else
				goto done;
		}
			break;
		default:
			break;
		};
	next:
		res = res->ai_next;
	}

done:
	freeaddrinfo(fr);
	return fd;
}

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

static void usage()
{
	printf("Usage: rpcping <protocol> <host> [--rpcbind] [--threads=<n>] [--count=<n>] [--port=<n>] [--program=<n>] [--version=<n>] [--procedure=<n>] [--nobind]\n");
}

int main(int argc, char *argv[])
{
	svc_init_params svc_params;
	CLIENT *clnt;
	struct state *s;
	struct state *states;
	double total;
	int i;
	char *proto;
	char *host;
	int nthreads;
	int count = 1500; /* observed optimal concurrent requests */
	int port = 2049;
	int prog = 100003; /* nfs */
	int vers = 3; /* allow raw, rdma, tcp, udp by default */
	int proc = 0;
	int send_sz = 8192;
	int recv_sz = 8192;
	int rpcbind = false;
	int opt;

	/* protocol and host/dest positional */
	if (argc < 3) {
		usage();
		exit(1);
	}

	proto = argv[1];
	host = argv[2];
	
	struct option long_options[] =
	{
		{"port", optional_argument, NULL, 'p'},
		{"threads", optional_argument, NULL, 't'},
		{"count", optional_argument, NULL, 'c'},
		{"program", optional_argument, NULL, 'm'},
		{"version", optional_argument, NULL, 'v'},
		{"procedure", optional_argument, NULL, 'x'},
		{"rpcbind", optional_argument, NULL, 'b'},
		{NULL, 0, NULL, 0}
	};

	optind = 3;
	while ((opt = getopt_long(argc, argv, "bt:c:p:m:v:s:", long_options,
						NULL))
		!= -1) {
		switch (opt)
		{
		case 't':
			nthreads = atoi(optarg);
			break;
		case 'c':
			count = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'm':
			prog = atoi(optarg);
			break;
		case 'v':
			vers = atoi(optarg);
			break;
		case 'x':
			proc = atoi(optarg);
			break;
		case 'b':
			rpcbind = true;
			break;
		default:
			usage();
			exit(1);
			break;
		};
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

		if (rpcbind) {
			clnt = clnt_ncreate(host, prog, vers, proto);
			if (CLNT_FAILURE(clnt)) {
				rpc_perror(&clnt->cl_error,
					"clnt_ncreate failed");
				exit(2);
			}
		} else {
			/* connect to host:port */
			struct sockaddr_storage ss;
			struct netbuf raddr = {
				.buf = &ss,
				.len = sizeof(ss)
			};
			int fd = get_conn_fd(host, port);
			if (fd <= 0) {
				perror("get_v4_conn failed");
				exit(3);
			}
			clnt = clnt_vc_ncreatef(fd, &raddr, prog, vers,
						send_sz,
						recv_sz,
						CLNT_CREATE_FLAG_CLOSE);
			if (CLNT_FAILURE(clnt)) {
				rpc_perror(&clnt->cl_error,
					"clnt_ncreate failed");
				exit(4);
			}
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

	fprintf(stdout, "rpcping %s %s threads=%d count=%d (port=%d program=%d version=%d procedure=%d): %2.4lf, total %2.4lf\n",
		proto, host, nthreads, count, port, prog, vers, proc,
		total / nthreads, total);
	fflush(stdout);
	return (0);
}
