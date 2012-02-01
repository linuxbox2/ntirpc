#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#if defined(TIRPC_EPOLL)
#include <sys/epoll.h>
#endif
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <rpc/types.h>

#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"

#include <rpc/svc.h>

#include <misc/rbtree_x.h>
#include "svc_xprt.h"
#include "vc_lock.h"

#define SVC_XPRT_PARTITIONS 17

static bool_t initialized = FALSE;

static struct svc_xprt_set svc_xprt_set = {
    PTHREAD_MUTEX_INITIALIZER /* svc_xprt_lock */,
    NULL /* xt */
};

void svc_xprt_init()
{
    int code = 0;

    mutex_lock(&svc_xprt_set.svc_xprt_lock);

    /* one of advantages of this RBT is convenience of external
     * iteration, we'll go to that shortly */
    code = rbtx_init(svc_xprt_set.xt, fd_cmpf /* NULL (inline) */,
                     SVC_XPRT_PARTITIONS, RBT_X_FLAG_ALLOC);
    if (code)
        __warnx("svc_xprt_init: rbtx_init failed");

    mutex_unlock(&svc_xprt_set.svc_xprt_lock);
}

#define cond_init_svc_xprt() { \
do { \
    if (! initialized) \
        svc_xprt_init(); \
    } while (0); \
}

SVCXPRT* svc_xprt_set_by_fd(int fd, SVCXPRT *xprt)
{
    SVCXPRT *xprt2 = NULL;

    /* XXXX implement */

    return (xprt2);
};

SVCXPRT* svc_xprt_lookup_by_fd(int fd)
{
    SVCXPRT *xprt = NULL;

    /* XXXX implement */

    return (xprt);    
}
