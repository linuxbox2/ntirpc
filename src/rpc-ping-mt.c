#include <rpc/rpc.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/times.h>
#include <pthread.h>
#include <signal.h>

static pthread_mutex_t rnmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rncond = PTHREAD_COND_INITIALIZER;
static volatile unsigned running;

struct state {
    unsigned long requests;
    CLIENT *handle;
    AUTH *auth;
    void *rpc_ctx;
    int id;
    int count;

    /* RPC */
    int proc;
    double averageTime;
};

static void worker_done() {
    pthread_mutex_lock(&rnmtx);
    --running;
    pthread_mutex_unlock(&rnmtx);
    pthread_cond_broadcast(&rncond);
}

void * worker(void *arg) {

    struct state *s = arg;
    enum clnt_stat status;
    clock_t rtime;
    struct tms dumm;
    struct timeval t;
    sigset_t mask, newmask;
    int i;

    sigfillset(&newmask);
    pthread_sigmask(SIG_SETMASK, &newmask, &mask);

    /*
     *   use 30 seconds timeout
     */
    t.tv_sec = 30;
    t.tv_usec = 0;

    rtime = times(&dumm);
    for (i = 0; i < s->count; i++) {
        status = clnt_vc_call_fast(s->handle, s->auth, s->proc,
				(xdrproc_t) xdr_void, NULL,
				(xdrproc_t) xdr_void, NULL, t,
				s->rpc_ctx);

        if (status == RPC_SUCCESS) {
            /* NOP */
        }
    }

    s->averageTime = s->count / ((double) (times(&dumm) - rtime) / (double) sysconf(_SC_CLK_TCK));

    auth_destroy(s->auth);
    clnt_destroy(s->handle);
    clnt_vc_put_fast_ctx(s->rpc_ctx);
    worker_done();
    return NULL;
}

int main(int argc, char *argv[]) {

    struct state *states, *s;

    int count = 100000;
    int i;
    int programm;
    int version;
    int nthreads = 1;
    double average;

    if (argc < 4 || argc > 5) {
        printf("Usage: rpcping <host> <program> <version> [nthreads] (e.g, rpcping localhost 100003 1)\n");
        exit(1);
    }

    if (argc == 5) {
        nthreads = atoi(argv[4]);
    }

    states = calloc(nthreads, sizeof (struct state));
    if (!states) {
        perror("malloc");
        exit(1);
    }

    /*
     *   Create Client Handle
     */
    programm = atoi(argv[2]);
    version = atoi(argv[3]);

    while (1) {
        running = nthreads;
        for (i = 0; i < nthreads; i++) {
            pthread_t t;
            s = &states[i];
            s->handle = clnt_create(argv[1], programm, version, "tcp");
            if (s->handle == NULL) {
                perror("clnt failed");
                exit(2);
            }
	    s->auth = authnone_ncreate();
	    s->rpc_ctx = clnt_vc_get_fast_ctx();

            s->id = i;
            s->count = count;
            s->proc = 0;
            pthread_create(&t, NULL, worker, s);
        }

        while (running) {
            pthread_cond_wait(&rncond, &rnmtx);
        }

        average = 0.0;
        for (i = 0; i < nthreads; i++) {
            s = &states[i];
            average += s->averageTime;
        }
        average = average / nthreads;
        fprintf(stdout, "Speed:  %2.4lf cps, %2.4lf cps in total\n",
		average, average * nthreads);
        fflush(stdout);
    }
}
