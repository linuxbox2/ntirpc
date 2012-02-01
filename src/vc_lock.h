
#ifndef TIRPC_VC_LOCK_H

/* clnt_fd_lock complex */
struct vc_fd_rec
{
    struct opr_rbtree_node node_k;
    int fd_k;
    int lock_flag_value; /* state of lock at fd */
    mutex_t mtx;
    cond_t cv;
}; 

struct vc_fd_rec_set
{
    mutex_t clnt_fd_lock; /* global mtx we'll try to spam less than formerly */
    struct rbtree_x *xt;
};

static inline int fd_cmpf(const struct opr_rbtree_node *lhs,
                          const struct opr_rbtree_node *rhs)
{
    struct vc_fd_rec *lk, *rk;

    lk = opr_containerof(lhs, struct vc_fd_rec, node_k);
    rk = opr_containerof(rhs, struct vc_fd_rec, node_k);

    if (lk->fd_k < rk->fd_k)
        return (-1);

    if (lk->fd_k == rk->fd_k)
        return (0);

    return (1);
}

#endif /* TIRPC_VC_LOCK_H */
