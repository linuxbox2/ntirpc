/*
 * Sun RPC is a product of Sun Microsystems, Inc. and is provided for
 * unrestricted use provided that this legend is included on all tape
 * media and as a part of the software program in whole or part.  Users
 * may copy or modify Sun RPC without charge, but are not authorized
 * to license or distribute it to anyone else except as part of a product or
 * program developed by the user.
 *
 * SUN RPC IS PROVIDED AS IS WITH NO WARRANTIES OF ANY KIND INCLUDING THE
 * WARRANTIES OF DESIGN, MERCHANTIBILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, OR ARISING FROM A COURSE OF DEALING, USAGE OR TRADE PRACTICE.
 *
 * Sun RPC is provided with no support and without any obligation on the
 * part of Sun Microsystems, Inc. to assist in its use, correction,
 * modification or enhancement.
 *
 * SUN MICROSYSTEMS, INC. SHALL HAVE NO LIABILITY WITH RESPECT TO THE
 * INFRINGEMENT OF COPYRIGHTS, TRADE SECRETS OR ANY PATENTS BY SUN RPC
 * OR ANY PART THEREOF.
 *
 * In no event will Sun Microsystems, Inc. be liable for any lost revenue
 * or profits or other special, indirect and consequential damages, even if
 * Sun has been advised of the possibility of such damages.
 *
 * Sun Microsystems, Inc.
 * 2550 Garcia Avenue
 * Mountain View, California  94043
 */

#include <sys/cdefs.h>

/*
 * Copyright (c) 1987 by Sun Microsystems, Inc.
 *
 * Portions Copyright(C) 1996, Jason Downs.  All rights reserved.
 */

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>

#include <string.h>

/*
 * Bind a socket to a privileged IP port
 */
int
bindresvport(sd, sin)
        int sd;
        struct sockaddr_in *sin;
{
        return bindresvport_sa(sd, (struct sockaddr *)sin);
}

#ifdef __linux__

#define STARTPORT 600
#define LOWPORT 512
#define ENDPORT (IPPORT_RESERVED - 1)
#define NPORTS  (ENDPORT - STARTPORT + 1)

int
bindresvport_sa(sd, sa)
        int sd;
        struct sockaddr *sa;
{
        int res, af;
        struct sockaddr_storage myaddr;
	struct sockaddr_in *sin;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	u_int16_t *portp;
	static u_int16_t port;
	static short startport = STARTPORT;
	socklen_t salen;
	int nports = ENDPORT - startport + 1;
	int endport = ENDPORT;
	int i;

        if (sa == NULL) {
                salen = sizeof(myaddr);
                sa = (struct sockaddr *)&myaddr;

                if (getsockname(sd, sa, &salen) == -1)
                        return -1;      /* errno is correctly set */

                af = sa->sa_family;
                memset(sa, 0, salen);
        } else
                af = sa->sa_family;

        switch (af) {
        case AF_INET:
		sin = (struct sockaddr_in *)sa;
                salen = sizeof(struct sockaddr_in);
                port = ntohs(sin->sin_port);
		portp = &sin->sin_port;
		break;
#ifdef INET6
        case AF_INET6:
		sin6 = (struct sockaddr_in6 *)sa;
                salen = sizeof(struct sockaddr_in6);
                port = ntohs(sin6->sin6_port);
                portp = &sin6->sin6_port;
                break;
#endif
        default:
                errno = EPFNOSUPPORT;
                return (-1);
        }
        sa->sa_family = af;

        if (port == 0) {
                port = (getpid() % NPORTS) + STARTPORT;
        }
        res = -1;
        errno = EADDRINUSE;
		again:
        for (i = 0; i < nports; ++i) {
                *portp = htons(port++);
                 if (port > endport) 
                        port = startport;
                res = bind(sd, sa, salen);
		if (res >= 0 || errno != EADDRINUSE)
	                break;
        }
	if (i == nports && startport != LOWPORT) {
	    startport = LOWPORT;
	    endport = STARTPORT - 1;
	    nports = STARTPORT - LOWPORT;
	    port = LOWPORT + port % (STARTPORT - LOWPORT);
	    goto again;
	}
        return (res);
}
#else

#define IP_PORTRANGE 19
#define IP_PORTRANGE_LOW 2

/*
 * Bind a socket to a privileged IP port
 */
int
bindresvport_sa(sd, sa)
	int sd;
	struct sockaddr *sa;
{
	int old, error, af;
	struct sockaddr_storage myaddr;
	struct sockaddr_in *sin;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif
	int proto, portrange, portlow;
	u_int16_t *portp;
	socklen_t salen;

	if (sa == NULL) {
		salen = sizeof(myaddr);
		sa = (struct sockaddr *)&myaddr;

		if (getsockname(sd, sa, &salen) == -1)
			return -1;	/* errno is correctly set */

		af = sa->sa_family;
		memset(sa, 0, salen);
	} else
		af = sa->sa_family;

	switch (af) {
	case AF_INET:
		proto = IPPROTO_IP;
		portrange = IP_PORTRANGE;
		portlow = IP_PORTRANGE_LOW;
		sin = (struct sockaddr_in *)sa;
		salen = sizeof(struct sockaddr_in);
		portp = &sin->sin_port;
		break;
#ifdef INET6
	case AF_INET6:
		proto = IPPROTO_IPV6;
		portrange = IPV6_PORTRANGE;
		portlow = IPV6_PORTRANGE_LOW;
		sin6 = (struct sockaddr_in6 *)sa;
		salen = sizeof(struct sockaddr_in6);
		portp = &sin6->sin6_port;
		break;
#endif
	default:
		errno = EPFNOSUPPORT;
		return (-1);
	}
	sa->sa_family = af;

	if (*portp == 0) {
		socklen_t oldlen = sizeof(old);

		error = getsockopt(sd, proto, portrange, &old, &oldlen);
		if (error < 0)
			return (error);

		error = setsockopt(sd, proto, portrange, &portlow,
		    sizeof(portlow));
		if (error < 0)
			return (error);
	}

	error = bind(sd, sa, salen);

	if (*portp == 0) {
		int saved_errno = errno;

		if (error < 0) {
			if (setsockopt(sd, proto, portrange, &old,
			    sizeof(old)) < 0)
				errno = saved_errno;
			return (error);
		}

		if (sa != (struct sockaddr *)&myaddr) {
			/* Hmm, what did the kernel assign? */
			if (getsockname(sd, sa, &salen) < 0)
				errno = saved_errno;
			return (error);
		}
	}
	return (error);
}
#endif
