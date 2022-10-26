/*
 * Copyright (c) 2012-2017 DESY Hamburg DMG-Division.
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * AUTHOR: Tigran Mkrtchayn (tigran.mkrtchyan@desy.de)
 * AUTHOR: William Allen Simpson <wsimpson@redhat.com>
 * AUTHOR: Matt Benjamin <mbenjamin@redhat.com>
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
#include "config.h"
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
#ifdef USE_LTTNG_NTIRPC
#include "lttng/rpcping.h"
#endif

static pthread_mutex_t rpcping_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rpcping_cond = PTHREAD_COND_INITIALIZER;
static uint32_t rpcping_threads;

static struct timespec to = {30, 0};

struct state {
	CLIENT *handle;
	pthread_cond_t s_cond;
	pthread_mutex_t s_mutex;
	struct timespec starting;
	struct timespec stopping;
	int count;
	int proc;
	int id;
	uint32_t failures;
	uint32_t responses;
	uint32_t timeouts;
};

static uint64_t timespec_elapsed(const struct timespec *starting,
				 const struct timespec *stopping)
{
	time_t elapsed = stopping->tv_sec - starting->tv_sec;
	long nsec = stopping->tv_nsec - starting->tv_nsec;

	return (elapsed * 1000000000L) + nsec;
}

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
			struct sockaddr_in *sin = (struct sockaddr_in *)
							res->ai_addr;

			sin->sin_port = htons(hbport);
			r = connect(fd, (struct sockaddr *) sin,
				    sizeof(struct sockaddr));
			if (!r) {
				goto done;
			}
			close(fd);
			break;
		}
		case AF_INET6:
		{
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)
							res->ai_addr;

			sin6->sin6_port = htons(hbport);
			r = connect(fd, (struct sockaddr *) sin6,
				    sizeof(struct sockaddr));
			if (!r) {
				goto done;
			}
			close(fd);
			break;
		}
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

	if (cc->cc_error.re_status != RPC_SUCCESS) {
		if (cc->cc_error.re_status == RPC_TIMEDOUT) {
			atomic_inc_uint32_t(&s->timeouts);
		} else {
			atomic_inc_uint32_t(&s->failures);
		}
	}

	clnt_req_release(cc);
	if (atomic_inc_uint32_t(&s->responses) < s->count) {
		return;
	}

	pthread_cond_broadcast(&s->s_cond);
}

static void *
worker(void *arg)
{
	struct state *s = arg;
	struct clnt_req *cc;
	int i;

	pthread_cond_init(&s->s_cond, NULL);
	pthread_mutex_init(&s->s_mutex, NULL);

	clock_gettime(CLOCK_MONOTONIC, &s->starting);
	for (i = 0; i < s->count; i++) {
		cc = calloc(1, sizeof(*cc));
		clnt_req_fill(cc, s->handle, authnone_ncreate(), s->proc,
			      (xdrproc_t) xdr_void, NULL,
			      (xdrproc_t) xdr_void, NULL);

		if (clnt_req_setup(cc, to) != RPC_SUCCESS) {
			rpc_perror(&cc->cc_error, "clnt_req_setup failed");
			s->count = i;
			clnt_req_release(cc);
			break;
		}
		cc->cc_refreshes = 1;
		cc->cc_process_cb = worker_cb;

		cc->cc_error.re_status = CLNT_CALL_BACK(cc);
		if (cc->cc_error.re_status != RPC_SUCCESS) {
			rpc_perror(&cc->cc_error, "CLNT_CALL_BACK failed");
			s->count = i;
			clnt_req_release(cc);
			break;
		}
	}

	pthread_mutex_lock(&s->s_mutex);
	pthread_cond_wait(&s->s_cond, &s->s_mutex);
	pthread_mutex_unlock(&s->s_mutex);
	clock_gettime(CLOCK_MONOTONIC, &s->stopping);

	if (atomic_dec_uint32_t(&rpcping_threads) > 0) {
		return NULL;
	}

	pthread_cond_broadcast(&rpcping_cond);
	return NULL;
}

static struct svc_req *
alloc_request(SVCXPRT *xprt, XDR *xdrs)
{
	struct svc_req *req = calloc(1, sizeof(*req));

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	req->rq_xprt = xprt;
	req->rq_xdrs = xdrs;
	req->rq_refcnt = 1;

	return req;
}

static void
free_request(struct svc_req *req, enum xprt_stat stat)
{
	free(req);
}

static void usage(void)
{
	printf("Usage: rpcping <raw|rdma|tcp|udp> <host> [--rpcbind] [--count=<n>] [--threads=<n>] [--workers=<n>] [--port=<n>] [--program=<n>] [--version=<n>] [--procedure=<n>]\n");
}

static struct option long_options[] =
{
	{"count", required_argument, NULL, 'c'},
	{"threads", required_argument, NULL, 't'},
	{"workers", required_argument, NULL, 'w'},
	{"port", required_argument, NULL, 'p'},
	{"program", required_argument, NULL, 'm'},
	{"version", required_argument, NULL, 'v'},
	{"procedure", required_argument, NULL, 'x'},
	{"rpcbind", no_argument, NULL, 'b'},
	{NULL, 0, NULL, 0}
};

int main(int argc, char *argv[])
{
	svc_init_params svc_params;
	CLIENT *clnt;
	struct state *s;
	struct state *states;
	char *proto;
	char *host;
	double total;
	double elapsed_ns;
	int i;
	int opt;
	int count = 500; /* minimal concurrent requests */
	int nthreads = 1;
	int nworkers = 5;
	int port = 2049;
	int prog = 100003; /* nfs */
	int vers = 3; /* allow raw, rdma, tcp, udp by default */
	int proc = 0;
	int send_sz = 8192;
	int recv_sz = 8192;
	unsigned int failures = 0;
	unsigned int timeouts = 0;
	bool rpcbind = false;

#ifdef USE_LTTNG_NTIRPC
	tracepoint(rpcping, test,
		   __FILE__, __func__, __LINE__, "Boo");
#endif
	/* protocol and host/dest positional */
	if (argc < 3) {
		usage();
		exit(1);
	}

	proto = argv[1];
	host = argv[2];

	optind = 3;
	while ((opt = getopt_long(argc, argv, "bc:m:p:t:v:w:x:",
				  long_options, NULL)) != -1) {
		switch (opt)
		{
		case 'c':
			count = atoi(optarg);
			break;
		case 't':
			nthreads = atoi(optarg);
			break;
		case 'w':
			nworkers = atoi(optarg);
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
	svc_params.alloc_cb = alloc_request;
	svc_params.free_cb = free_request;
	svc_params.flags = SVC_INIT_EPOLL | SVC_INIT_NOREG_XPRTS;
	svc_params.max_events = 512;
	svc_params.ioq_thrd_max = nworkers;

	if (!svc_init(&svc_params)) {
		perror("svc_init failed");
		exit(1);
	}

	rpcping_threads = nthreads;
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
				perror("get_conn_fd failed");
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
	}

	pthread_mutex_lock(&rpcping_mutex);
	pthread_cond_wait(&rpcping_cond, &rpcping_mutex);
	pthread_mutex_unlock(&rpcping_mutex);

	total = 0.0;
	elapsed_ns = 0.0;
	for (i = 0; i < nthreads; i++) {
		s = &states[i];
		failures += s->failures;
		timeouts += s->timeouts;
		total += s->responses;
		elapsed_ns += timespec_elapsed(&s->starting, &s->stopping);
		CLNT_DESTROY(s->handle);
	}
	total *= 1000000000.0;
	total /= elapsed_ns;

	fprintf(stdout, "rpcping %s %s count=%d threads=%d workers=%d (port=%d program=%d version=%d procedure=%d): failures %u timeouts %u mean %2.4lf, total %2.4lf\n",
		proto, host, count, nthreads, nworkers, port, prog, vers, proc,
		failures, timeouts, total / nthreads, total);
	fflush(stdout);

	(void)svc_shutdown(SVC_SHUTDOWN_FLAG_NONE);
	return (0);
}
