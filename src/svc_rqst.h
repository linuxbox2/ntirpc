
#ifndef TIRPC_SVC_RQST_H
#define TIRPC_SVC_RQST_H

struct svc_rqst_rec
{
    /*
     * union of event processor types
     */
    enum svc_event_type ev_type;
    union {
        struct {
            int epoll_fd;
            struct epoll_event *events;
            u_int max_events; /* max epoll events */
        } epoll;
        struct {
            fd_set set; /* select/fd_set (currently unhooked) */
        } fd;
    } ev_u;

    uint32_t id_k; /* id */
    struct opr_queue xprt_q; /* list of xprt handles */
    
    struct opr_rbtree_node node_k;
    uint64_t gen; /* generation number */
    mutex_t mtx;
};

struct svc_rqst_set
{
    rwlock_t lock;
    struct opr_rbtree t;
};

static inline int rqst_xprt_cmpf(const struct opr_rbtree_node *lhs,
                                 const struct opr_rbtree_node *rhs)
{
    struct svc_rqst_rec *lk, *rk;

    lk = opr_containerof(lhs, struct svc_rqst_rec, node_k);
    rk = opr_containerof(rhs, struct svc_rqst_rec, node_k);

    if (lk->id_k < rk->id_k)
        return (-1);

    if (lk->id_k == rk->id_k)
        return (0);

    return (1);
}

/*
 * exported interface:
 *
 *  svc_rqst_init -- init module (optional)
 *  svc_rqst_register_thrd -- create thread dispatcher slot
 *  svc_rqst_unregister_thrd -- delete thread dispatcher slot
 *  svc_rqst_register_thrd_xprt -- set {xprt, dispatcher} mapping 
 *  svc_rqst_unregister_thrd_xprt -- unset {xprt, dispatcher} mapping
 *  svc_rqst_foreach_xprt -- scan registered xprts at id (or 0 for all)
 *  svc_rqst_thrd_run -- enter dispatch loop at id
 *  svc_rqst_thrd_signal --request thread to run a callout function which
 *   can cause the thread to return
 *  svc_rqst_shutdown -- cause all threads to return
 *
 * callback function interface:
 *
 * svc_xprt_rendezvous -- called when a new transport connection is accepted,
 *  can be used by the application to chose a correct request handler, or do
 *  other adaptation
 */

int svc_rqst_register_thrd(uint32_t *id /* OUT */, void *u_data,
                            uint32_t flags);
int svc_rqst_unregister_thrd(uint32_t id, uint32_t flags);
int svc_rqst_register_thrd_xprt(uint32_t id, SVCXPRT *xprt, uint32_t flags);
int svc_rqst_unregister_thrd_xprt(uint32_t id, SVCXPRT *xprt, uint32_t flags);
int svc_rqst_thrd_run(uint32_t id, SVCXPRT *xprt, uint32_t flags);

/* xprt/connection rendezvous callout */
typedef int (*svc_rqst_rendezvous_t)
    (SVCXPRT *oxprt, SVCXPRT *nxprt, uint32_t flags);

/* iterator callback prototype */
typedef void (*svc_rqst_xprt_each_func_t)
    (uint32_t id, SVCXPRT *xprt, void *arg);
int svc_rqst_foreach_xprt(svc_rqst_xprt_each_func_t each_f, uint32_t id,
                          void *arg);

#endif /* TIRPC_SVC_RQST_H */
