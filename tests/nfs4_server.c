#include <rpc/rpc.h>
#include <rpc/clnt.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <unistd.h>
#include <errno.h>

#include <mooshika.h>

#include "rpc/svc.h"

#include "nfsv40.h"
#include "nfs4.h"
#include "fsal_nfsv4_macros.h"


extern SVCXPRT *svc_msk_create(msk_trans_t*, u_int, void (*)(void*), void*);


#define PROGNUM 100003
#define NFS_V4 4
#define SENDSIZE 32768
#define RECVSIZE 32768
#define CREDITS 20

inline void die(char* str, int i) {
	printf("%s: %s (%d)\n", str, strerror(i), i);
	exit(1);
}

int main() {
	int rc;
	SVCXPRT *rpc_svc;

	msk_trans_t *trans;
        msk_trans_attr_t attr;


       	memset(&attr, 0, sizeof(msk_trans_attr_t));

	attr.server = 10;
	attr.rq_depth = CREDITS;
	attr.sq_depth = CREDITS;
	attr.max_send_sge = 4;
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(20049);
	attr.addr.sa_in.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (msk_init(&trans, &attr))
		die("couldn't init trans", ENOMEM);

	msk_bind_server(trans);

	trans = msk_accept_one(trans);
	rpc_svc = svc_msk_create(trans, CREDITS, NULL, NULL);

	if (!rpc_svc)
		die("no rpc client", errno);


	struct rpc_msg rply;
	struct svc_req req;

	FILE *logfd =	fopen("/tmp/nfsrdma_log", "w+");

	while(1) {
		memset(&req, 0, sizeof(req));
		memset(&rply, 0, sizeof(rply));

		rc = rpc_svc->xp_ops->xp_recv(rpc_svc, &req);
		printf("Got something (status %d)\n", rc);
		fwrite((char*)(&req.rq_msg), sizeof(req.rq_msg), sizeof(char), logfd);
	        fwrite("\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11\x11", 0x10, sizeof(char), logfd);
		fflush(logfd);
		rply.rm_xid = req.rq_xid;
		rply.rm_direction=REPLY;
		rply.rm_reply.rp_stat = MSG_DENIED;
		rply.rm_flags = RPC_MSG_FLAG_NONE;
		rply.rjcted_rply.rj_stat = AUTH_ERROR;
		rply.rjcted_rply.rj_why = AUTH_FAILED;

		rpc_svc->xp_ops->xp_reply(rpc_svc, &req, &rply);

	}



	msk_destroy_trans(&trans);

	return 0;
}
