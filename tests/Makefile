GANESHA_BUILD=/opt/GANESHA/build-nfs
CFLAGS=-g -Wall -Werror -I../ntirpc
LDFLAGS=-L$(GANESHA_BUILD)/libntirpc/src

all: nfs4_testmsk nfs4_server

nfs4_testmsk: nfs4_testmsk.c nfs4_xdr.o
	gcc $(CFLAGS) $(LDFLAGS) nfs4_xdr.o nfs4_testmsk.c  -o nfs4_testmsk -lntirpc -lmooshika -lrt -lpthread -lgssapi_krb5

nfs4_server: nfs4_server.c nfs4_xdr.o
	gcc $(CFLAGS) $(LDFLAGS) nfs4_xdr.o nfs4_server.c  -o nfs4_server -lntirpc -lmooshika -lrt -lpthread -lgssapi_krb5

#ignore CFLAGS for that one...
nfs4_xdr.o: nfs4_xdr.c
	gcc -g -I../tirpc -c nfs4_xdr.c

clean:
	rm -f *.o nfs4_{testmsk,server}
