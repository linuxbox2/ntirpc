
#ifndef TIRPC_SVC_XPRT_H
#define TIRPC_SVC_XPRT_H

struct svc_xprt_rec
{
    struct opr_rbtree_node node_k;
    int fd_k;
    SVCXPRT *xprt;
    uint64_t gen; /* generation number */
    mutex_t mtx;
};

struct svc_xprt_set
{
    mutex_t lock;
    struct rbtree_x xt;
};

SVCXPRT* svc_xprt_set(SVCXPRT *xprt);
SVCXPRT* svc_xprt_get(int fd);
SVCXPRT *svc_xprt_clear(SVCXPRT *xprt);

/* iterator callback prototype */
typedef void (*svc_xprt_each_func_t) (SVCXPRT *xprt, void *arg);
int svc_xprt_foreach(svc_xprt_each_func_t each_f, void *arg);
void svc_xprt_dump_xprts(const char *tag);
void svc_xprt_shutdown();

#endif /* TIRPC_SVC_XPRT_H */
