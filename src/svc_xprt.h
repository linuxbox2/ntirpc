
#ifndef TIRPC_XVC_XPRT_H

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
    struct rbtree_x *xt;
};

SVCXPRT* svc_xprt_set(SVCXPRT *xprt);
SVCXPRT* svc_xprt_get(int fd);

#endif /* TIRPC_XVC_XPRT_H */
