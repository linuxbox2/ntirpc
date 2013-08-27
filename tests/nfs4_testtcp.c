#include <rpc/rpc.h>
#include <rpc/clnt.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <unistd.h>
#include <errno.h>

#include "rpc/clnt.h"

//#include "nfsv40.h"
#include "nfs4.h"
#include "fsal_nfsv4_macros.h"

#define PROGNUM 100003
#define NFS_V4 4
#define SENDSIZE 32768
#define RECVSIZE 32768

inline void die(char* str, int i) {
	printf("%s: %s (%d)\n", str, strerror(i), i);
	exit(1);
}


int main() {
	int sock;
  	int priv_port = 0;
	int rc;
	struct sockaddr_in addr_rpc;
	CLIENT *rpc_client;

	memset(&addr_rpc, 0, sizeof(addr_rpc));
	addr_rpc.sin_port = htons(2049);
	addr_rpc.sin_family = AF_INET;
	addr_rpc.sin_addr.s_addr = inet_addr("127.0.0.1");

	if ((sock = rresvport ( &priv_port)) < 0)
		die("rresvport failed", errno);

	if ((rc = connect(sock, (struct sockaddr *)&addr_rpc, sizeof(addr_rpc))) < 0)
		die("connect failed", errno);

//	rpc_client = clnttcp_create(&addr_rpc, PROGNUM, NFS_V4, &sock, SENDSIZE, RECVSIZE);
	struct netbuf svcaddr;
	svcaddr.maxlen = svcaddr.len = sizeof(struct sockaddr_in);
	svcaddr.buf = &addr_rpc;
	rpc_client = clnt_vc_create(sock, &svcaddr, PROGNUM, NFS_V4, SENDSIZE, RECVSIZE);

	if (!rpc_client)
		die("no rpc client", errno);

	AUTH *auth = authunix_ncreate_default();

	struct timeval timeout = TIMEOUTRPC;

	rc = clnt_call(rpc_client, auth, NFSPROC4_NULL,
		(xdrproc_t) xdr_void, (caddr_t) NULL,
		(xdrproc_t) xdr_void, (caddr_t) NULL, timeout);

	if (rc != RPC_SUCCESS)
		printf("procnull went wrong...\n");
	else
		printf("procnull success!\n");



	COMPOUND4args argnfs4;
	COMPOUND4res resnfs4;

#define FSAL_LOOKUP_NB_OP_ALLOC 3
	nfs_argop4 argoparray[FSAL_LOOKUP_NB_OP_ALLOC];
	nfs_resop4 resoparray[FSAL_LOOKUP_NB_OP_ALLOC];
	memset(argoparray, 0, sizeof(nfs_argop4)*FSAL_LOOKUP_NB_OP_ALLOC);
	memset(resoparray, 0, sizeof(nfs_resop4)*FSAL_LOOKUP_NB_OP_ALLOC);



	/* Setup results structures */
	argnfs4.argarray.argarray_val = argoparray;
	resnfs4.resarray.resarray_val = resoparray;
	argnfs4.minorversion = 0;
	argnfs4.argarray.argarray_len = 0;

	argnfs4.tag.utf8string_val = NULL;
	argnfs4.tag.utf8string_len = 0;


  bitmap4 bitmap;
  uint32_t bitmap_val[2];
  bitmap.bitmap4_val = bitmap_val;
  bitmap.bitmap4_len = 2;

  bitmap.bitmap4_val[0] = 0b01000000000100011010;
  bitmap.bitmap4_val[1] = 0b00111010;


#define FSAL_LOOKUP_IDX_OP_PUTROOTFH      0
#define	FSAL_LOOKUP_IDX_OP_GETATTR_ROOT   1
#define FSAL_LOOKUP_IDX_OP_GETFH_ROOT     2
	COMPOUNDV4_ARG_ADD_OP_PUTROOTFH(argnfs4);
	COMPOUNDV4_ARG_ADD_OP_GETATTR(argnfs4, bitmap);
	COMPOUNDV4_ARG_ADD_OP_GETFH(argnfs4);


#define FSAL_PROXY_FILEHANDLE_MAX_LEN 256
	char padfilehandle[FSAL_PROXY_FILEHANDLE_MAX_LEN];
	memset(padfilehandle, 0, FSAL_PROXY_FILEHANDLE_MAX_LEN);




	resnfs4.resarray.resarray_val[FSAL_LOOKUP_IDX_OP_GETFH_ROOT].nfs_resop4_u.opgetfh.
	    GETFH4res_u.resok4.object.nfs_fh4_val = (char *)padfilehandle;
	resnfs4.resarray.resarray_val[FSAL_LOOKUP_IDX_OP_GETFH_ROOT].nfs_resop4_u.opgetfh.
	    GETFH4res_u.resok4.object.nfs_fh4_len = FSAL_PROXY_FILEHANDLE_MAX_LEN;

	rc = clnt_call(rpc_client, auth, NFSPROC4_COMPOUND,
		(xdrproc_t) xdr_COMPOUND4args, (caddr_t) &argnfs4,
		(xdrproc_t) xdr_COMPOUND4res, (caddr_t) &resnfs4, timeout);



	if (rc != RPC_SUCCESS)
		printf("compound went wrong... %s (%u)\n", clnt_sperrno(rc), rc);
	else
		printf("got root fh : %llx\n", *(long long unsigned int*)padfilehandle); // use %s?

	close(sock);

	return 0;
}
