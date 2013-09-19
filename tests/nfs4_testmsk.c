#include <rpc/rpc.h>
#include <rpc/clnt.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <unistd.h>
#include <errno.h>

#include <mooshika.h>

#include "rpc/clnt.h"

//#include "nfsv40.h"
#include "nfs4.h"
#include "fsal_nfsv4_macros.h"

/*
 * Mooshika clnt create routine
 */
extern CLIENT *clnt_msk_create(msk_trans_t *,
                              const rpcprog_t, const rpcvers_t,
                              u_int);


#define PROGNUM 100003
#define NFS_V4 4
#define SENDSIZE 32768
#define RECVSIZE 32768
#define CREDITS 20

inline void die(char* str, int i) {
	printf("%s: %s (%d)\n", str, strerror(i), i);
	exit(1);
}

void nfs4_list_to_bitmap4(bitmap4 * b, uint32_t plen, uint32_t * pval)
{
  uint32_t i;
  int maxpos =  -1;

  memset(b->bitmap4_val, 0, sizeof(uint32_t)*b->bitmap4_len);

  for(i = 0; i < plen; i++)
    {
      int intpos = pval[i] / 32;
      int bitpos = pval[i] % 32;

      if(intpos	>= b->bitmap4_len)
        {
          printf("Mismatch between bitmap len and the list: "
                  "got %d, need %d to accomodate attribute %d\n",
                  b->bitmap4_len, intpos+1, pval[i]);
          continue;
        }
      b->bitmap4_val[intpos] |= (1U << bitpos);
      if(intpos > maxpos)
        maxpos = intpos;
    }

  b->bitmap4_len = maxpos + 1;
}                              	/* nfs4_list_to_bitmap4 */


void fsal_internal_proxy_create_fattr_readdir_bitmap(bitmap4 * pbitmap)
{
  uint32_t tmpattrlist[20];
  uint32_t attrlen = 0;
                            
  pbitmap->bitmap4_len = 2;
  memset(pbitmap->bitmap4_val, 0, sizeof(uint32_t) * pbitmap->bitmap4_len);

  tmpattrlist[0] = FATTR4_TYPE;
  tmpattrlist[1] = FATTR4_CHANGE;
  tmpattrlist[2] = FATTR4_SIZE;
  tmpattrlist[3] = FATTR4_FSID;
  tmpattrlist[4] = FATTR4_FILEHANDLE;
  tmpattrlist[5] = FATTR4_FILEID;
  tmpattrlist[6] = FATTR4_MODE;
  tmpattrlist[7] = FATTR4_NUMLINKS;
  tmpattrlist[8] = FATTR4_OWNER;
  tmpattrlist[9] = FATTR4_OWNER_GROUP;
  tmpattrlist[10] = FATTR4_SPACE_USED;
  tmpattrlist[11] = FATTR4_TIME_ACCESS;
  tmpattrlist[12] = FATTR4_TIME_METADATA;
  tmpattrlist[13] = FATTR4_TIME_MODIFY;
  tmpattrlist[14] = FATTR4_RAWDEV;

  attrlen = 15;

  nfs4_list_to_bitmap4(pbitmap,	attrlen, tmpattrlist);

}      	                        /* fsal_internal_proxy_create_fattr_readdir_bitmap */



void fsal_internal_proxy_setup_readdir_fattr(fsal_proxy_internal_fattr_readdir_t * pfattr)
{
  /* Just do the correct connection */
  pfattr->owner.utf8string_val = pfattr->padowner;
  pfattr->owner_group.utf8string_val = pfattr->padgroup;
  pfattr->filehandle.nfs_fh4_val = pfattr->padfh;
}



int main() {
	int rc;
	CLIENT *rpc_client;

	msk_trans_t *trans;
        msk_trans_attr_t attr;


       	memset(&attr, 0, sizeof(msk_trans_attr_t));

	attr.server = 0;
	attr.rq_depth = CREDITS;
	attr.sq_depth = CREDITS;
	attr.max_send_sge = 4;
	attr.port = "20049";
	attr.node = "::1";

	if (msk_init(&trans, &attr))
		die("couldn't init trans", ENOMEM);

	rpc_client = clnt_msk_create(trans, PROGNUM, NFS_V4, CREDITS);

	if (!rpc_client)
		die("no rpc client", errno);

	AUTH *auth = authunix_ncreate_default();


	struct timeval timeout = TIMEOUTRPC;

#if 0
	rc = clnt_call(rpc_client, auth, NFSPROC4_NULL,
		(xdrproc_t) xdr_void, (caddr_t) NULL,
		(xdrproc_t) xdr_void, (caddr_t) NULL, timeout);

	if (rc != RPC_SUCCESS)
		printf("procnull went wrong (%d): %s", rc, clnt_sperrno(rc));
	else
		printf("procnull success!\n");

#endif // 0

	COMPOUND4args argnfs4;
	COMPOUND4res resnfs4;

#define NB_OP_ALLOC 2
	nfs_argop4 argoparray[NB_OP_ALLOC];
	nfs_resop4 resoparray[NB_OP_ALLOC];
	memset(argoparray, 0, sizeof(nfs_argop4)*NB_OP_ALLOC);
	memset(resoparray, 0, sizeof(nfs_resop4)*NB_OP_ALLOC);


	/* Setup results structures */
	argnfs4.argarray.argarray_val = argoparray;
	resnfs4.resarray.resarray_val = resoparray;
	argnfs4.minorversion = 0;
	argnfs4.argarray.argarray_len = 0;

	/* argnfs4.tag.utf8string_val = "GANESHA NFSv4 Proxy: Lookup Root" ; */
	argnfs4.tag.utf8string_val = NULL;
	argnfs4.tag.utf8string_len = 0;


	int nbreaddir = 256;
	proxyfsal_cookie_t start_position;
	start_position.data = 0;
	bitmap4 bitmap;
	uint32_t bitmap_val[2];
	bitmap.bitmap4_len = 2;
	bitmap.bitmap4_val = bitmap_val;
	fsal_internal_proxy_create_fattr_readdir_bitmap(&bitmap);

	verifier4 verifier;
	memset(verifier, 0, sizeof(verifier));

#define IDX_OP_PUTROOTFH      0
#define IDX_OP_READDIR        1
	COMPOUNDV4_ARG_ADD_OP_PUTROOTFH(argnfs4);
	COMPOUNDV4_ARG_ADD_OP_READDIR(argnfs4, start_position.data, nbreaddir, verifier, bitmap);


	struct proxy_entry4 {
		entry4 e4;
		char name[MAXNAMLEN];
		fsal_proxy_internal_fattr_readdir_t attr;
		uint32_t bitmap[2];
	} *pxy_e4;
	pxy_e4 = calloc(nbreaddir, sizeof(*pxy_e4));
	if (!pxy_e4)
		die("calloc failed", ENOMEM);

	memset(pxy_e4, 0, sizeof(*pxy_e4)*nbreaddir);

	int i;

	for (i=0; i<nbreaddir; i++) {
		fsal_internal_proxy_setup_readdir_fattr(&pxy_e4[i].attr);
		pxy_e4[i].e4.name.utf8string_val = pxy_e4[i].name;
		pxy_e4[i].e4.name.utf8string_len = sizeof(pxy_e4[i].name);

		pxy_e4[i].e4.attrs.attr_vals.attrlist4_val = (char *)&(pxy_e4[i].attr);
		pxy_e4[i].e4.attrs.attr_vals.attrlist4_len = sizeof(pxy_e4[i].attr);

		pxy_e4[i].e4.attrs.attrmask.bitmap4_val = pxy_e4[i].bitmap;
		pxy_e4[i].e4.attrs.attrmask.bitmap4_len = 2;

		pxy_e4[i].e4.nextentry = &pxy_e4[i+1].e4; /* Last one cleared after the loop */
	}
	pxy_e4[i-1].e4.nextentry = NULL;
	resnfs4.resarray.resarray_val[IDX_OP_READDIR].nfs_resop4_u.opreaddir.READDIR4res_u.resok4.reply.entries = & pxy_e4->e4;
	



	rc = clnt_call(rpc_client, auth, NFSPROC4_COMPOUND,
		(xdrproc_t) xdr_COMPOUND4args, (caddr_t) &argnfs4,
		(xdrproc_t) xdr_COMPOUND4res, (caddr_t) &resnfs4, timeout);



	if (rc != RPC_SUCCESS)
		printf("compound went wrong (%d): %s\n", rc, clnt_sperrno(rc));
	else
		printf("compound: got status : %x\n", 	resnfs4.resarray.resarray_val[IDX_OP_PUTROOTFH].nfs_resop4_u.opputrootfh.status); // use %s?

	msk_destroy_trans(&trans);

	return 0;
}
