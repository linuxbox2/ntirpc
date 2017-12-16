
#include <config.h>
#include <sys/cdefs.h>
#include <pthread.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#include "rpc_com.h"

/* protects the services list (svc.c) */
pthread_rwlock_t svc_lock = RWLOCK_INITIALIZER;

/* protects the RPCBIND address cache */
pthread_rwlock_t rpcbaddr_cache_lock = RWLOCK_INITIALIZER;

#ifdef KERBEROS
/* auth_kerb.c serialization */
pthread_mutex_t authkerb_lock = MUTEX_INITIALIZER;
/* protects kerb stats list */
pthread_mutex_t svcauthkerbstats_lock = MUTEX_INITIALIZER;
#endif				/* KERBEROS */

/* protects the Auths list (svc_auth.c) */
pthread_mutex_t authsvc_lock = MUTEX_INITIALIZER;

/* domainname and domain_fd (getdname.c) and default_domain (rpcdname.c) */
pthread_mutex_t dname_lock = MUTEX_INITIALIZER;

/* protects first_time and hostname (key_call.c) */
pthread_mutex_t keyserv_lock = MUTEX_INITIALIZER;

/* serializes rpc_trace() (rpc_trace.c) */
pthread_mutex_t libnsl_trace_lock = MUTEX_INITIALIZER;

/* loopnconf (rpcb_clnt.c) */
pthread_mutex_t loopnconf_lock = MUTEX_INITIALIZER;

/* serializes ops initializations */
pthread_mutex_t ops_lock = MUTEX_INITIALIZER;

/* protects ``port'' static in bindresvport() */
pthread_mutex_t portnum_lock = MUTEX_INITIALIZER;

/* protects proglst list (svc_simple.c) */
pthread_mutex_t proglst_lock = MUTEX_INITIALIZER;

/* svc_raw.c serialization */
pthread_mutex_t svcraw_lock = MUTEX_INITIALIZER;

/* protects TSD key creation */
pthread_mutex_t tsd_lock = MUTEX_INITIALIZER;

/* Library global tsd keys */
thread_key_t rpc_call_key = -1;
thread_key_t tcp_key = -1;
thread_key_t udp_key = -1;
thread_key_t nc_key = -1;
thread_key_t vsock_key = -1;

/* xprtlist (svc_generic.c) */
pthread_mutex_t xprtlist_lock = MUTEX_INITIALIZER;

/* serializes calls to public key routines */
pthread_mutex_t serialize_pkey = MUTEX_INITIALIZER;

void tsd_key_delete(void)
{
	if (rpc_call_key != -1)
		pthread_key_delete(rpc_call_key);
	if (tcp_key != -1)
		pthread_key_delete(tcp_key);
	if (udp_key != -1)
		pthread_key_delete(udp_key);
	if (nc_key != -1)
		pthread_key_delete(nc_key);
	return;
}
