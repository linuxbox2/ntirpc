
#ifndef TIRPC_XVC_XPRT_H

struct svc_xprt_rec
{
    struct opr_rbtree_node node_k;
    int fd_k;
    SVCXPRT *xprt;
    uint64_t gen; /* generation number */
};

struct svc_xprt_set
{
    mutex_t svc_xprt_lock; /* XXX unused */
    struct rbtree_x *xt;
};

#endif /* TIRPC_XVC_XPRT_H */
