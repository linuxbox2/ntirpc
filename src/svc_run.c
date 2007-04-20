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

/*
 * This is the rpc server side idle loop
 * Wait for input, call server program.
 */
#include <pthread.h>
#include <reentrant.h>
#include <err.h>
#include <errno.h>
#include <rpc/rpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>
#include "rpc_com.h"
#include <sys/select.h>

void
svc_run()
{
	fd_set readfds, cleanfds;
	struct timeval timeout;
	extern rwlock_t svc_fd_lock;

	timeout.tv_sec = 30;
	timeout.tv_usec = 0;

	for (;;) {
		rwlock_rdlock(&svc_fd_lock);
		readfds = svc_fdset;
		cleanfds = svc_fdset;
		rwlock_unlock(&svc_fd_lock);
		switch (select(svc_maxfd+1, &readfds, NULL, NULL, &timeout)) {
		case -1:
			FD_ZERO(&readfds);
			if (errno == EINTR) {
				continue;
			}
			warn("svc_run: - select failed");
			return;
		case 0:
			__svc_clean_idle(&cleanfds, 30, FALSE);
			continue;
		default:
			svc_getreqset(&readfds);
		}
	}
}

/*
 *      This function causes svc_run() to exit by telling it that it has no
 *      more work to do.
 */
void
svc_exit()
{
	extern rwlock_t svc_fd_lock;

	rwlock_wrlock(&svc_fd_lock);
	FD_ZERO(&svc_fdset);
	rwlock_unlock(&svc_fd_lock);
}
