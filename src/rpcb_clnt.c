/*
 * Copyright (c) 2010, Oracle America, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the "Oracle America, Inc." nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rpcb_clnt.c
 * interface to rpcbind rpc service.
 */
#include <config.h>
#include <pthread.h>
#include <reentrant.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <rpc/rpc.h>
#include <rpc/rpcb_prot.h>
#include <rpc/nettype.h>
#include <netconfig.h>
#ifdef PORTMAP
#include <netinet/in.h>		/* FOR IPPROTO_TCP/UDP definitions */
#include <rpc/pmap_prot.h>
#endif				/* PORTMAP */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <syslog.h>
#include <assert.h>

#include "rpc_com.h"

static struct timeval tottimeout = { 60, 0 };
static const struct timeval rmttimeout = { 3, 0 };
static struct timeval rpcbrmttime = { 15, 0 };

extern bool xdr_wrapstring(XDR *, char **);

static const char nullstring[] = "\000";

#define RPCB_OWNER_STRING "libntirpc"

#define CACHESIZE 6

struct address_cache {
	char *ac_host;
	char *ac_netid;
	char *ac_uaddr;
	struct netbuf *ac_taddr;
	struct address_cache *ac_next;
};

static struct address_cache *front;
static int cachesize;

#define CLCR_GET_RPCB_TIMEOUT 1
#define CLCR_SET_RPCB_TIMEOUT 2

extern int __rpc_lowvers;

static struct address_cache *check_cache(const char *, const char *);
static void delete_cache(struct netbuf *);
static void add_cache(const char *, const char *, struct netbuf *, char *);
static CLIENT *getclnthandle(const char *, const struct netconfig *, char **);
static CLIENT *local_rpcb(void);
#ifdef NOTUSED
static struct netbuf *got_entry(rpcb_entry_list_ptr, const struct netconfig *);
#endif

/*
 * This routine adjusts the timeout used for calls to the remote rpcbind.
 * Also, this routine can be used to set the use of portmapper version 2
 * only when doing rpc_broadcasts
 * These are private routines that may not be provided in future releases.
 */
bool __rpc_control(int request, void *info)
{
	switch (request) {
	case CLCR_GET_RPCB_TIMEOUT:
		*(struct timeval *)info = tottimeout;
		break;
	case CLCR_SET_RPCB_TIMEOUT:
		tottimeout = *(struct timeval *)info;
		break;
	case CLCR_SET_LOWVERS:
		__rpc_lowvers = *(int *)info;
		break;
	case CLCR_GET_LOWVERS:
		*(int *)info = __rpc_lowvers;
		break;
	default:
		return (false);
	}
	return (true);
}

/*
 * It might seem that a reader/writer lock would be more reasonable here.
 * However because getclnthandle(), the only user of the cache functions,
 * may do a delete_cache() operation if a check_cache() fails to return an
 * address useful to clnt_tli_ncreate(), we may as well use a mutex.
 */
/*
 * As it turns out, if the cache lock is *not* a reader/writer lock, we will
 * block all clnt_ncreate's if we are trying to connect to a host that's down,
 * since the lock will be held all during that time.
 */
extern rwlock_t rpcbaddr_cache_lock;

/*
 * The routines check_cache(), add_cache(), delete_cache() manage the
 * cache of rpcbind addresses for (host, netid).
 */

static struct address_cache *
check_cache(const char *host, const char *netid)
{
	struct address_cache *cptr;

	/* READ LOCK HELD ON ENTRY: rpcbaddr_cache_lock */
	for (cptr = front; cptr != NULL; cptr = cptr->ac_next) {
		if (!strcmp(cptr->ac_host, host)
		    && !strcmp(cptr->ac_netid, netid)) {
#ifdef ND_DEBUG
			fprintf(stderr, "Found cache entry for %s: %s\n", host,
				netid);
#endif
			return (cptr);
		}
	}
	return ((struct address_cache *)NULL);
}

static void
delete_cache(struct netbuf *addr)
{
	struct address_cache *cptr, *prevptr = NULL;

	/* WRITE LOCK HELD ON ENTRY: rpcbaddr_cache_lock */
	for (cptr = front; cptr != NULL; cptr = cptr->ac_next) {
		if (!memcmp(cptr->ac_taddr->buf, addr->buf, addr->len)) {
			mem_free(cptr->ac_host, 0);	/* XXX */
			mem_free(cptr->ac_netid, 0);
			mem_free(cptr->ac_taddr->buf, cptr->ac_taddr->len);
			mem_free(cptr->ac_taddr, sizeof(struct netbuf));
			if (cptr->ac_uaddr)
				mem_free(cptr->ac_uaddr, 0);
			if (prevptr)
				prevptr->ac_next = cptr->ac_next;
			else
				front = cptr->ac_next;
			mem_free(cptr, sizeof(struct address_cache));
			cachesize--;
			break;
		}
		prevptr = cptr;
	}
}

static void
add_cache(const char *host, const char *netid, struct netbuf *taddr,
	  char *uaddr)
{
	struct address_cache *ad_cache, *cptr, *prevptr;

	if (!host) {
		__warnx(TIRPC_DEBUG_FLAG_ERROR, "%s: missing host", __func__);
		return;
	}
	ad_cache = (struct address_cache *)mem_zalloc(sizeof(*ad_cache));

	ad_cache->ac_host = mem_strdup(host);
	ad_cache->ac_netid = mem_strdup(netid);
	ad_cache->ac_uaddr = uaddr ? mem_strdup(uaddr) : NULL;
	ad_cache->ac_taddr = (struct netbuf *)mem_zalloc(sizeof(struct netbuf));
	ad_cache->ac_taddr->len = ad_cache->ac_taddr->maxlen = taddr->len;
	ad_cache->ac_taddr->buf = (char *)mem_zalloc(taddr->len);
	memcpy(ad_cache->ac_taddr->buf, taddr->buf, taddr->len);
#ifdef ND_DEBUG
	fprintf(stderr, "Added to cache: %s : %s\n", host, netid);
#endif

	/* VARIABLES PROTECTED BY rpcbaddr_cache_lock:  cptr */
	rwlock_wrlock(&rpcbaddr_cache_lock);
	if (cachesize < CACHESIZE) {
		ad_cache->ac_next = front;
		front = ad_cache;
		cachesize++;
	} else {
		/* Free the last entry */
		cptr = front;
		prevptr = NULL;
		while (cptr->ac_next) {
			prevptr = cptr;
			cptr = cptr->ac_next;
		}

#ifdef ND_DEBUG
		fprintf(stderr, "Deleted from cache: %s : %s\n", cptr->ac_host,
			cptr->ac_netid);
#endif
		mem_free(cptr->ac_host, 0);	/* XXX */
		mem_free(cptr->ac_netid, 0);
		mem_free(cptr->ac_taddr->buf, cptr->ac_taddr->len);
		mem_free(cptr->ac_taddr, sizeof(struct netbuf));
		if (cptr->ac_uaddr)
			mem_free(cptr->ac_uaddr, 0);

		if (prevptr) {
			prevptr->ac_next = NULL;
			ad_cache->ac_next = front;
			front = ad_cache;
		} else {
			front = ad_cache;
			ad_cache->ac_next = NULL;
		}
		mem_free(cptr, sizeof(struct address_cache));
	}
	rwlock_unlock(&rpcbaddr_cache_lock);
	return;
}

/*
 * This routine will return a client handle that is connected to the
 * rpcbind. If targaddr is non-NULL, the "universal address" of the
 * host will be stored in *targaddr; the caller is responsible for
 * freeing this string.
 * On error, returns NULL and free's everything.
 */
static CLIENT *getclnthandle(const char *host, const struct netconfig *nconf,
			     char **targaddr)
{
	CLIENT *client;
	struct netbuf *addr, taddr;
	struct netbuf addr_to_delete;
	struct __rpc_sockinfo si;
	struct addrinfo hints, *res, *tres;
	struct address_cache *ad_cache;
	char *tmpaddr;

	memset(&addr_to_delete, '\0', sizeof(addr_to_delete));

/* VARIABLES PROTECTED BY rpcbaddr_cache_lock:  ad_cache */

	/* Get the address of the rpcbind.  Check cache first */
	client = NULL;
	if (targaddr)
		*targaddr = NULL;
	addr_to_delete.len = 0;
	rwlock_rdlock(&rpcbaddr_cache_lock);
	ad_cache = NULL;
	if (host != NULL)
		ad_cache = check_cache(host, nconf->nc_netid);
	if (ad_cache != NULL) {
		addr = ad_cache->ac_taddr;
		client =
		    clnt_tli_ncreate(RPC_ANYFD, nconf, addr,
				     (rpcprog_t) RPCBPROG,
				     (rpcvers_t) RPCBVERS4, 0, 0);
		if (client != NULL) {
			if (targaddr)
				*targaddr = mem_strdup(ad_cache->ac_uaddr);
			rwlock_unlock(&rpcbaddr_cache_lock);
			return (client);
		}
		addr_to_delete.len = addr->len;
		addr_to_delete.buf = (char *)mem_zalloc(addr->len);
		memcpy(addr_to_delete.buf, addr->buf, addr->len);
	}
	rwlock_unlock(&rpcbaddr_cache_lock);
	if (addr_to_delete.len != 0) {
		/*
		 * Assume this may be due to cache data being
		 *  outdated
		 */
		rwlock_wrlock(&rpcbaddr_cache_lock);
		delete_cache(&addr_to_delete);
		rwlock_unlock(&rpcbaddr_cache_lock);
		mem_free(addr_to_delete.buf, addr_to_delete.len);
	}
	if (!__rpc_nconf2sockinfo(nconf, &si)) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		assert(client == NULL);
		goto out_err;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = si.si_af;
	hints.ai_socktype = si.si_socktype;
	hints.ai_protocol = si.si_proto;

#ifdef CLNT_DEBUG
	__warnx("%s: trying netid %s family %d proto %d socktype %d\n",
		__func__, nconf->nc_netid, si.si_af, si.si_proto,
		si.si_socktype);
#endif

	if (strcmp(nconf->nc_protofmly, NC_LOOPBACK) == 0) {
		client = local_rpcb();
		if (!client) {
#ifdef ND_DEBUG
			clnt_pcreateerror("rpcbind clnt interface");
#endif
			goto out_err;
		} else {
			struct sockaddr_un sun;

			if (targaddr) {
				*targaddr = mem_zalloc(sizeof(sun.sun_path));
				strncpy(*targaddr, _PATH_RPCBINDSOCK,
					sizeof(sun.sun_path));
			}
			return (client);
		}
	} else {
		if (getaddrinfo(host, "sunrpc", &hints, &res) != 0) {
			rpc_createerr.cf_stat = RPC_UNKNOWNHOST;
			assert(client == NULL);
			goto out_err;
		}
	}

	for (tres = res; tres != NULL; tres = tres->ai_next) {
		taddr.buf = tres->ai_addr;
		taddr.len = taddr.maxlen = tres->ai_addrlen;

#ifdef ND_DEBUG
		{
			char *ua;

			ua = taddr2uaddr(nconf, &taddr);
			fprintf(stderr, "Got it [%s]\n", ua);
			mem_free(ua, 0);	/* XXX */
		}
#endif

#ifdef ND_DEBUG
		{
			int i;

			fprintf(stderr, "\tnetbuf len = %d, maxlen = %d\n",
				taddr.len, taddr.maxlen);
			fprintf(stderr, "\tAddress is ");
			for (i = 0; i < taddr.len; i++)
				fprintf(stderr, "%u.",
					((char *)(taddr.buf))[i]);
			fprintf(stderr, "\n");
		}
#endif
		client =
		    clnt_tli_ncreate(RPC_ANYFD, nconf, &taddr,
				     (rpcprog_t) RPCBPROG,
				     (rpcvers_t) RPCBVERS4, 0, 0);
#ifdef ND_DEBUG
		if (!client)
			clnt_pcreateerror("rpcbind clnt interface");
#endif

		if (client) {
			tmpaddr = targaddr ? taddr2uaddr(nconf, &taddr) : NULL;
			add_cache(host, nconf->nc_netid, &taddr, tmpaddr);
			if (targaddr)
				*targaddr = tmpaddr;
			break;
		}
	}
	if (res)
		freeaddrinfo(res);
 out_err:
	if (!client && targaddr)
		mem_free(*targaddr, 0);
	return (client);
}

/*
 * Set a mapping between program, version and address.
 * Calls the rpcbind service to do the mapping.
 */
bool
rpcb_set(rpcprog_t program, rpcvers_t version, const struct netconfig *nconf,
	 /* Network structure of transport */
	 const struct netbuf *address /* Services netconfig address */)
{
	RPCB parms;
	char uidbuf[32];
	bool_t rslt = false;	/* yes, bool_t */
	CLIENT *client;
	AUTH *auth;

	/* parameter checking */
	if (nconf == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (false);
	}
	if (address == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR;
		return (false);
	}
	client = local_rpcb();
	if (!client)
		return (false);

	auth = authnone_ncreate();	/* idempotent */

	/* convert to universal */
	/*LINTED const castaway */
	parms.r_addr =
	    taddr2uaddr((struct netconfig *)nconf, (struct netbuf *)address);
	if (!parms.r_addr) {
		CLNT_DESTROY(client);
		rpc_createerr.cf_stat = RPC_N2AXLATEFAILURE;
		return (false);	/* no universal address */
	}
	parms.r_prog = program;
	parms.r_vers = version;
	parms.r_netid = nconf->nc_netid;
	/*
	 * Though uid is not being used directly, we still send it for
	 * completeness.  For non-unix platforms, perhaps some other
	 * string or an empty string can be sent.
	 */
	(void)snprintf(uidbuf, sizeof(uidbuf), "%d", geteuid());
	parms.r_owner = uidbuf;

	CLNT_CALL(client, auth, RPCBPROC_SET, (xdrproc_t) xdr_rpcb,
		  (char *)&parms, (xdrproc_t) xdr_bool, (char *)&rslt,
		  tottimeout);

	CLNT_DESTROY(client);
	mem_free(parms.r_addr, 0);
	return (rslt);
}

/*
 * Remove the mapping between program, version and netbuf address.
 * Calls the rpcbind service to do the un-mapping.
 * If netbuf is NULL, unset for all the transports, otherwise unset
 * only for the given transport.
 */
bool
rpcb_unset(rpcprog_t program, rpcvers_t version,
	   const struct netconfig *nconf)
{
	RPCB parms;
	char uidbuf[32];
	bool_t rslt = false;	/* yes, bool_t */
	CLIENT *client;
	AUTH *auth;

	client = local_rpcb();
	if (!client)
		return (false);
	auth = authnone_ncreate();	/* idempotent */
	parms.r_prog = program;
	parms.r_vers = version;
	if (nconf)
		parms.r_netid = nconf->nc_netid;
	else {
		/*LINTED const castaway */
		parms.r_netid = (char *)&nullstring[0];	/* unsets  all */
	}
	/*LINTED const castaway */
	parms.r_addr = (char *)&nullstring[0];
	(void)snprintf(uidbuf, sizeof(uidbuf), "%d", geteuid());
	parms.r_owner = uidbuf;

	CLNT_CALL(client, auth, RPCBPROC_UNSET, (xdrproc_t) xdr_rpcb,
		  (char *)(void *)&parms, (xdrproc_t) xdr_bool,
		  (char *)(void *)&rslt, tottimeout);

	CLNT_DESTROY(client);
	return (rslt);
}

#ifdef NOTUSED
/*
 * From the merged list, find the appropriate entry
 */
static struct netbuf *
got_entry(rpcb_entry_list_ptr relp,
	  const struct netconfig *nconf)
{
	struct netbuf *na = NULL;
	rpcb_entry_list_ptr sp;
	rpcb_entry *rmap;

	for (sp = relp; sp != NULL; sp = sp->rpcb_entry_next) {
		rmap = &sp->rpcb_entry_map;
		if ((strcmp(nconf->nc_proto, rmap->r_nc_proto) == 0)
		    && (strcmp(nconf->nc_protofmly, rmap->r_nc_protofmly) == 0)
		    && (nconf->nc_semantics == rmap->r_nc_semantics)
		    && (rmap->r_maddr != NULL) && (rmap->r_maddr[0] != 0)) {
			na = uaddr2taddr(nconf, rmap->r_maddr);
#ifdef ND_DEBUG
			fprintf(stderr, "\tRemote address is [%s].\n",
				rmap->r_maddr);
			if (!na)
				fprintf(stderr,
					"\tCouldn't resolve remote address!\n");
#endif
			break;
		}
	}
	return (na);
}
#endif

/*
 * Quick check to see if rpcbind is up.  Tries to connect over
 * local transport.
 */
bool
__rpcbind_is_up(void)
{
	struct netconfig *nconf;
	struct sockaddr_un sun;
	void *localhandle;
	int sock;

	nconf = NULL;
	localhandle = setnetconfig();
	if (localhandle == NULL)
		return (false);

	while ((nconf = getnetconfig(localhandle)) != NULL) {
		if (nconf->nc_protofmly != NULL
		    && strcmp(nconf->nc_protofmly, NC_LOOPBACK) == 0)
			break;
	}
	endnetconfig(localhandle);
	if (nconf == NULL)
		return (false);

	memset(&sun, 0, sizeof(sun));
	sock = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0)
		return (false);
	sun.sun_family = AF_LOCAL;
	strncpy(sun.sun_path, _PATH_RPCBINDSOCK, sizeof(sun.sun_path));

	if (connect(sock, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		close(sock);
		return (false);
	}

	close(sock);
	return (true);
}

/*
 * An internal function which optimizes rpcb_getaddr function.  It also
 * returns the client handle that it uses to contact the remote rpcbind.
 *
 * The algorithm used: If the transports is TCP or UDP, it first tries
 * version 2 (portmap), 4 and then 3 (svr4).  This order should be
 * changed in the next OS release to 4, 2 and 3.  We are assuming that by
 * that time, version 4 would be available on many machines on the network.
 * With this algorithm, we get performance as well as a plan for
 * obsoleting version 2.
 *
 * For all other transports, the algorithm remains as 4 and then 3.
 *
 * XXX: Due to some problems with t_connect(), we do not reuse the same client
 * handle for COTS cases and hence in these cases we do not return the
 * client handle.  This code will change if t_connect() ever
 * starts working properly.  Also look under clnt_vc.c.
 */
struct netbuf *
__rpcb_findaddr_timed(rpcprog_t program, rpcvers_t version,
		      const struct netconfig *nconf,
		      const char *host, CLIENT **clpp,
		      struct timeval *tp)
{
#ifdef NOTUSED
	static bool check_rpcbind = true;
#endif
	RPCB parms;
	enum clnt_stat clnt_st;
	char *ua = NULL;
	rpcvers_t vers;
	struct netbuf *address = NULL;
	rpcvers_t start_vers = RPCBVERS4;
	struct netbuf servaddr;
	CLIENT *client = NULL;
	AUTH *auth;

	/* parameter checking */
	if (nconf == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (NULL);
	}

	parms.r_addr = NULL;

	/* authnone handle */
	auth = authnone_ncreate();	/* idempotent */

	/*
	 * Use default total timeout if no timeout is specified.
	 */
	if (tp == NULL)
		tp = &tottimeout;

#ifdef PORTMAP
	/* Try version 2 for TCP or UDP */
	if (strcmp(nconf->nc_protofmly, NC_INET) == 0) {
		u_short port = 0;
		struct netbuf remote;
		rpcvers_t pmapvers = 2;
		struct pmap pmapparms;

		/*
		 * Try UDP only - there are some portmappers out
		 * there that use UDP only.
		 */
		if (strcmp(nconf->nc_proto, NC_TCP) == 0) {
			struct netconfig *newnconf;

			newnconf = getnetconfigent("udp");
			if (!newnconf) {
				rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
				return (NULL);
			}
			client = getclnthandle(host, newnconf, &parms.r_addr);
			freenetconfigent(newnconf);
		} else if (strcmp(nconf->nc_proto, NC_UDP) == 0)
			client = getclnthandle(host, nconf, &parms.r_addr);
		else
			goto try_rpcbind;
		if (client == NULL)
			return (NULL);

		/*
		 * Set version and retry timeout.
		 */
		CLNT_CONTROL(client, CLSET_RETRY_TIMEOUT, (char *)&rpcbrmttime);
		CLNT_CONTROL(client, CLSET_VERS, (char *)&pmapvers);

		pmapparms.pm_prog = program;
		pmapparms.pm_vers = version;
		pmapparms.pm_prot =
		    strcmp(nconf->nc_proto, NC_TCP) ? IPPROTO_UDP : IPPROTO_TCP;
		pmapparms.pm_port = 0;	/* not needed */
		clnt_st =
		    CLNT_CALL(client, auth, PMAPPROC_GETPORT,
			      (xdrproc_t) xdr_pmap,
			      (caddr_t) (void *)&pmapparms,
			      (xdrproc_t) xdr_u_short, (caddr_t) (void *)&port,
			      *tp);
		if (clnt_st != RPC_SUCCESS) {
			if ((clnt_st == RPC_PROGVERSMISMATCH)
			    || (clnt_st == RPC_PROGUNAVAIL))
				goto try_rpcbind;
			rpc_createerr.cf_stat = RPC_PMAPFAILURE;
			clnt_geterr(client, &rpc_createerr.cf_error);
			goto error;
		} else if (port == 0) {
			address = NULL;
			rpc_createerr.cf_stat = RPC_PROGNOTREGISTERED;
			goto error;
		}
		port = htons(port);
		CLNT_CONTROL(client, CLGET_SVC_ADDR, (char *)&remote);

		address = (struct netbuf *)mem_zalloc(sizeof(struct netbuf));
		address->buf = (char *)mem_alloc(remote.len);

		memcpy(address->buf, remote.buf, remote.len);
		memcpy(&((char *)address->buf)[sizeof(short)],
		       (char *)(void *)&port, sizeof(short));
		address->len = address->maxlen = remote.len;
		goto done;
	}

 try_rpcbind:
#endif				/* PORTMAP */

	parms.r_prog = program;
	parms.r_vers = version;
	parms.r_netid = nconf->nc_netid;

	/*
	 * rpcbind ignores the r_owner field in GETADDR requests, but we
	 * need to give xdr_rpcb something to gnaw on. Might as well make
	 * it something human readable for when we see these in captures.
	 */
	parms.r_owner = RPCB_OWNER_STRING;

	/* Now the same transport is to be used to get the address */
	if (client && ((nconf->nc_semantics == NC_TPI_COTS_ORD)
		       || (nconf->nc_semantics == NC_TPI_COTS))) {
		/* A CLTS type of client - destroy it */
		CLNT_DESTROY(client);
		client = NULL;
		mem_free(parms.r_addr, 0);
		parms.r_addr = NULL;
	}

	if (client == NULL) {
		client = getclnthandle(host, nconf, &parms.r_addr);
		if (client == NULL)
			goto error;
	}
	if (parms.r_addr == NULL) {
		/*LINTED const castaway */
		parms.r_addr = (char *)&nullstring[0];
	}

	/* First try from start_vers(4) and then version 3 (RPCBVERS) */

	CLNT_CONTROL(client, CLSET_RETRY_TIMEOUT, (char *)&rpcbrmttime);
	for (vers = start_vers; vers >= RPCBVERS; vers--) {
		/* Set the version */
		CLNT_CONTROL(client, CLSET_VERS, (char *)(void *)&vers);
		clnt_st =
		    CLNT_CALL(client, auth, RPCBPROC_GETADDR,
			      (xdrproc_t) xdr_rpcb, (char *)(void *)&parms,
			      (xdrproc_t) xdr_wrapstring, (char *)(void *)&ua,
			      *tp);
		if (clnt_st == RPC_SUCCESS) {
			if ((ua == NULL) || (ua[0] == 0)) {
				/* address unknown */
				rpc_createerr.cf_stat = RPC_PROGNOTREGISTERED;
				goto error;
			}
			address = uaddr2taddr(nconf, ua);
#ifdef ND_DEBUG
			fprintf(stderr, "\tRemote address is [%s]\n", ua);
			if (!address)
				fprintf(stderr,
					"\tCouldn't resolve remote address!\n");
#endif
			xdr_free((xdrproc_t) xdr_wrapstring,
				 (char *)(void *)&ua);

			if (!address) {
				/* We don't know about your universal address */
				rpc_createerr.cf_stat = RPC_N2AXLATEFAILURE;
				goto error;
			}
			CLNT_CONTROL(client, CLGET_SVC_ADDR,
				     (char *)(void *)&servaddr);
			__rpc_fixup_addr(address, &servaddr);
			goto done;
		} else if (clnt_st == RPC_PROGVERSMISMATCH) {
			struct rpc_err rpcerr;
			clnt_geterr(client, &rpcerr);
			if (rpcerr.re_vers.low > RPCBVERS4)
				goto error; /* a new version, can't handle */
		} else if (clnt_st != RPC_PROGUNAVAIL) {
			/* Cant handle this error */
			rpc_createerr.cf_stat = clnt_st;
			clnt_geterr(client, &rpc_createerr.cf_error);
			goto error;
		}
	}

	if ((address == NULL) || (address->len == 0)) {
		rpc_createerr.cf_stat = RPC_PROGNOTREGISTERED;
		clnt_geterr(client, &rpc_createerr.cf_error);
	}

 error:
	if (client) {
		CLNT_DESTROY(client);
		client = NULL;
	}
 done:
	if (nconf->nc_semantics != NC_TPI_CLTS) {
		/* This client is the connectionless one */
		if (client) {
			CLNT_DESTROY(client);
			client = NULL;
		}
	}
	if (clpp)
		*clpp = client;
	else if (client)
		CLNT_DESTROY(client);
	if (parms.r_addr != NULL && parms.r_addr != nullstring)
		mem_free(parms.r_addr, 0);
	return (address);
}

/*
 * Helper routine to find mapped address (for NLM).
 */
extern struct netbuf *
rpcb_find_mapped_addr(char *nettype, rpcprog_t prog,
		      rpcvers_t vers, char *local_addr)
{
	struct netbuf *nbuf;
	void *handle = __rpc_setconf(nettype);
	struct netconfig *nconf = __rpc_getconf(handle);
	nbuf = __rpcb_findaddr_timed(prog, vers, nconf, local_addr, NULL, NULL);
	__rpc_endconf(handle);
	return (nbuf);
}

/*
 * Find the mapped address for program, version.
 * Calls the rpcbind service remotely to do the lookup.
 * Uses the transport specified in nconf.
 * Returns false (0) if no map exists, else returns 1.
 *
 * Assuming that the address is all properly allocated
 */
bool
rpcb_getaddr(rpcprog_t program, rpcvers_t version,
	     const struct netconfig *nconf, struct netbuf *address,
	     const char *host)
{
	struct netbuf *na;

	na = __rpcb_findaddr_timed(
		program, version, (struct netconfig *)nconf,
		(char *)host, (CLIENT **) NULL,
		(struct timeval *)NULL);
	if (!na)
		return (false);

	if (na->len > address->maxlen) {
		/* Too long address */
		mem_free(na->buf, 0);
		mem_free(na, 0);
		rpc_createerr.cf_stat = RPC_FAILED;
		return (false);
	}
	memcpy(address->buf, na->buf, (size_t) na->len);
	address->len = na->len;
	mem_free(na->buf, 0);
	mem_free(na, 0);
	return (true);
}

/*
 * Get a copy of the current maps.
 * Calls the rpcbind service remotely to get the maps.
 *
 * It returns only a list of the services
 * It returns NULL on failure.
 */
rpcblist *
rpcb_getmaps(const struct netconfig *nconf, const char *host)
{
	rpcblist_ptr head = NULL;
	enum clnt_stat clnt_st;
	rpcvers_t vers = 0;
	CLIENT *client;
	AUTH *auth;

	client = getclnthandle(host, nconf, NULL);
	if (client == NULL)
		return (head);

	auth = authnone_ncreate();	/* idempotent */

	clnt_st =
	    CLNT_CALL(client, auth, RPCBPROC_DUMP, (xdrproc_t) xdr_void, NULL,
		      (xdrproc_t) xdr_rpcblist_ptr, (char *)(void *)&head,
		      tottimeout);
	if (clnt_st == RPC_SUCCESS)
		goto done;

	if ((clnt_st != RPC_PROGVERSMISMATCH) && (clnt_st != RPC_PROGUNAVAIL)) {
		rpc_createerr.cf_stat = RPC_RPCBFAILURE;
		clnt_geterr(client, &rpc_createerr.cf_error);
		goto done;
	}

	/* fall back to earlier version */
	CLNT_CONTROL(client, CLGET_VERS, (char *)(void *)&vers);
	if (vers == RPCBVERS4) {
		vers = RPCBVERS;
		CLNT_CONTROL(client, CLSET_VERS, (char *)(void *)&vers);
		if (CLNT_CALL
		    (client, auth, RPCBPROC_DUMP, (xdrproc_t) xdr_void, NULL,
		     (xdrproc_t) xdr_rpcblist_ptr, (char *)(void *)&head,
		     tottimeout) == RPC_SUCCESS)
			goto done;
	}
	rpc_createerr.cf_stat = RPC_RPCBFAILURE;
	clnt_geterr(client, &rpc_createerr.cf_error);

 done:
	CLNT_DESTROY(client);
	return (head);
}

/*
 * rpcbinder remote-call-service interface.
 * This routine is used to call the rpcbind remote call service
 * which will look up a service program in the address maps, and then
 * remotely call that routine with the given parameters. This allows
 * programs to do a lookup and call in one step.
 */
enum clnt_stat
rpcb_rmtcall(const struct netconfig *nconf, /* Netconfig structure */
	     const char *host, /* Remote host name */
	     rpcprog_t prog, rpcvers_t vers,
	     rpcproc_t proc,/* Remote proc identifiers */
	     xdrproc_t xdrargs, caddr_t argsp, xdrproc_t xdrres,
	     caddr_t resp,	/* Argument and Result */
	     struct timeval tout,	/* Timeout value for this call */
	     const struct netbuf *addr_ptr
	     /* Preallocated netbuf address */)
{
	enum clnt_stat stat;
	struct r_rpcb_rmtcallargs a;
	struct r_rpcb_rmtcallres r;
	rpcvers_t rpcb_vers;
	CLIENT *client;
	AUTH *auth;

	stat = 0;
	client = getclnthandle(host, nconf, NULL);
	if (client == NULL)
		return (RPC_FAILED);

	auth = authnone_ncreate();	/* idempotent */

	/*LINTED const castaway */
	CLNT_CONTROL(client, CLSET_RETRY_TIMEOUT, (char *)(void *)&rmttimeout);
	a.prog = prog;
	a.vers = vers;
	a.proc = proc;
	a.args.args_val = argsp;
	a.xdr_args = xdrargs;
	r.addr = NULL;
	r.results.results_val = resp;
	r.xdr_res = xdrres;

	for (rpcb_vers = RPCBVERS4; rpcb_vers >= RPCBVERS; rpcb_vers--) {
		CLNT_CONTROL(client, CLSET_VERS, (char *)(void *)&rpcb_vers);
		stat =
		    CLNT_CALL(client, auth, RPCBPROC_CALLIT,
			      (xdrproc_t) xdr_rpcb_rmtcallargs,
			      (char *)(void *)&a,
			      (xdrproc_t) xdr_rpcb_rmtcallres,
			      (char *)(void *)&r, tout);
		if ((stat == RPC_SUCCESS) && (addr_ptr != NULL)) {
			struct netbuf *na;
			/*LINTED const castaway */
			na = uaddr2taddr((struct netconfig *)nconf, r.addr);
			if (!na) {
				stat = RPC_N2AXLATEFAILURE;
				/*LINTED const castaway */
				((struct netbuf *)addr_ptr)->len = 0;
				goto error;
			}
			if (na->len > addr_ptr->maxlen) {
				/* Too long address */
				stat = RPC_FAILED; /* XXX A better error no */
				mem_free(na->buf, 0);	/* XXX */
				mem_free(na, 0);
				/*LINTED const castaway */
				((struct netbuf *)addr_ptr)->len = 0;
				goto error;
			}
			memcpy(addr_ptr->buf, na->buf, (size_t) na->len);
			/*LINTED const castaway */
			((struct netbuf *)addr_ptr)->len = na->len;
			mem_free(na->buf, 0);
			mem_free(na, 0);
			break;
		} else if ((stat != RPC_PROGVERSMISMATCH)
			   && (stat != RPC_PROGUNAVAIL)) {
			goto error;
		}
	}
 error:
	CLNT_DESTROY(client);
	if (r.addr)
		xdr_free((xdrproc_t) xdr_wrapstring, (char *)(void *)&r.addr);
	return (stat);
}

/*
 * Gets the time on the remote host.
 * Returns 1 if succeeds else 0.
 */
int
boolrpcb_gettime(const char *host, time_t *timep)
{
	void *handle;
	struct netconfig *nconf;
	rpcvers_t vers;
	enum clnt_stat st;
	CLIENT *client = NULL;
	AUTH *auth;

	if ((host == NULL) || (host[0] == 0)) {
		time(timep);
		return (true);
	}

	handle = __rpc_setconf("netpath");
	if (!handle) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (false);
	}
	rpc_createerr.cf_stat = RPC_SUCCESS;
	while (client == NULL) {
		nconf = __rpc_getconf(handle);
		if (!nconf) {
			if (rpc_createerr.cf_stat == RPC_SUCCESS)
				rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			break;
		}
		client = getclnthandle(host, nconf, NULL);
		if (client)
			break;
	}
	__rpc_endconf(handle);
	if (client == (CLIENT *) NULL)
		return (false);

	auth = authnone_ncreate();	/* idempotent */

	st = CLNT_CALL(client, auth, RPCBPROC_GETTIME, (xdrproc_t) xdr_void,
		       NULL, (xdrproc_t) xdr_int, (char *)(void *)timep,
		       tottimeout);

	if ((st == RPC_PROGVERSMISMATCH) || (st == RPC_PROGUNAVAIL)) {
		CLNT_CONTROL(client, CLGET_VERS, (char *)(void *)&vers);
		if (vers == RPCBVERS4) {
			/* fall back to earlier version */
			vers = RPCBVERS;
			CLNT_CONTROL(client, CLSET_VERS, (char *)(void *)&vers);
			st = CLNT_CALL(client, auth, RPCBPROC_GETTIME,
				       (xdrproc_t) xdr_void, NULL,
				       (xdrproc_t) xdr_int,
				       (char *)(void *)timep, tottimeout);
		}
	}
	CLNT_DESTROY(client);
	return (st == RPC_SUCCESS ? true : false);
}

/*
 * Converts taddr to universal address.  This routine should never
 * really be called because local n2a libraries are always provided.
 */
char *rpcb_taddr2uaddr(struct netconfig *nconf, struct netbuf *taddr)
{
	CLIENT *client;
	AUTH *auth = authnone_ncreate();
	char *uaddr = NULL;

	/* parameter checking */
	if (nconf == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (NULL);
	}
	if (taddr == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR;
		return (NULL);
	}
	client = local_rpcb();
	if (!client)
		return (NULL);
	CLNT_CALL(client, auth, RPCBPROC_TADDR2UADDR, (xdrproc_t) xdr_netbuf,
		  (char *)(void *)taddr, (xdrproc_t) xdr_wrapstring,
		  (char *)(void *)&uaddr, tottimeout);
	CLNT_DESTROY(client);
	return (uaddr);
}

/*
 * Converts universal address to netbuf.  This routine should never
 * really be called because local n2a libraries are always provided.
 */
struct netbuf *rpcb_uaddr2taddr(struct netconfig *nconf, char *uaddr)
{
	struct netbuf *taddr;
	CLIENT *client;
	AUTH *auth;

	/* parameter checking */
	if (nconf == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
		return (NULL);
	}
	if (uaddr == NULL) {
		rpc_createerr.cf_stat = RPC_UNKNOWNADDR;
		return (NULL);
	}
	client = local_rpcb();
	if (!client)
		return (NULL);

	auth = authnone_ncreate();	/* idempotent */

	taddr = (struct netbuf *)mem_zalloc(sizeof(struct netbuf));
	if (CLNT_CALL
	    (client, auth, RPCBPROC_UADDR2TADDR, (xdrproc_t) xdr_wrapstring,
	     (char *)(void *)&uaddr, (xdrproc_t) xdr_netbuf,
	     (char *)(void *)taddr, tottimeout) != RPC_SUCCESS) {
		mem_free(taddr, sizeof(*taddr));
		taddr = NULL;
	}
	CLNT_DESTROY(client);
	return (taddr);
}

/* XXX */
#define IN4_LOCALHOST_STRING "127.0.0.1"
#define IN6_LOCALHOST_STRING "::1"

/*
 * This routine will return a client handle that is connected to the local
 * rpcbind. Returns NULL on error and free's everything.
 */
static CLIENT *local_rpcb(void)
{
	CLIENT *client;
	static struct netconfig *loopnconf;
	static char *hostname;
	extern mutex_t loopnconf_lock;
	int sock;
	size_t tsize;
	struct netbuf nbuf;
	struct sockaddr_un sun;

	/*
	 * Try connecting to the local rpcbind through a local socket
	 * first. If this doesn't work, try all transports defined in
	 * the netconfig file.
	 */
	memset(&sun, 0, sizeof(sun));
	sock = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0)
		goto try_nconf;
	sun.sun_family = AF_LOCAL;
	strcpy(sun.sun_path, _PATH_RPCBINDSOCK);
	nbuf.len = SUN_LEN(&sun);
	nbuf.maxlen = sizeof(struct sockaddr_un);
	nbuf.buf = &sun;

	tsize = __rpc_get_t_size(AF_LOCAL, 0, 0);
	client =
	    clnt_vc_ncreate(sock, &nbuf, (rpcprog_t) RPCBPROG,
			    (rpcvers_t) RPCBVERS, tsize, tsize);

	if (client != NULL) {
		/* Mark the socket to be closed in destructor */
		(void)CLNT_CONTROL(client, CLSET_FD_CLOSE, NULL);
		return client;
	}

	/* Nobody needs this socket anymore; free the descriptor. */
	close(sock);

 try_nconf:
	/* VARIABLES PROTECTED BY loopnconf_lock: loopnconf */
	mutex_lock(&loopnconf_lock);
	if (loopnconf == NULL) {
		struct netconfig *nconf, *tmpnconf = NULL;
		void *nc_handle;
		int fd;

		nc_handle = setnetconfig();
		if (nc_handle == NULL) {
			/* fails to open netconfig file */
			syslog(LOG_ERR, "rpc: failed to open " NETCONFIG);
			rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			mutex_unlock(&loopnconf_lock);
			return (NULL);
		}
		while ((nconf = getnetconfig(nc_handle)) != NULL) {
#ifdef INET6
			if ((strcmp(nconf->nc_protofmly, NC_INET6) == 0 ||
#else
			if ((
#endif
				    strcmp(nconf->nc_protofmly, NC_INET) == 0)
			    && (nconf->nc_semantics == NC_TPI_COTS
				|| nconf->nc_semantics == NC_TPI_COTS_ORD)) {
				fd = __rpc_nconf2fd(nconf);
				/*
				 * Can't create a socket, assume that
				 * this family isn't configured in the kernel.
				 */
				if (fd < 0)
					continue;
				close(fd);
				tmpnconf = nconf;
				if (!strcmp(nconf->nc_protofmly, NC_INET))
					hostname = IN4_LOCALHOST_STRING;
				else
					hostname = IN6_LOCALHOST_STRING;
			}
		}
		if (tmpnconf == NULL) {
			rpc_createerr.cf_stat = RPC_UNKNOWNPROTO;
			endnetconfig(nc_handle);
			mutex_unlock(&loopnconf_lock);
			return (NULL);
		}
		loopnconf = getnetconfigent(tmpnconf->nc_netid);
		/* loopnconf is never freed */
		endnetconfig(nc_handle);
	}
	mutex_unlock(&loopnconf_lock);
	client = getclnthandle(hostname, loopnconf, NULL);
	return (client);
}
