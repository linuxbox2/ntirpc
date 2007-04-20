#	from: @(#)Makefile	2.3 88/08/11 4.0 RPCSRC
#	$FreeBSD: src/include/rpc/Makefile,v 1.2 2002/05/12 16:00:48 ru Exp $

#.SUFFIXES: .x

#RPCCOM = rpcgen -C

all:: crypt.h
	@:

RPCHDRS= crypt.h

XFILES= crypt.x

#CLEANFILES+= ${HDRS}

#INCS=	${HFILES} ${XFILES} ${HDRS}
#INCSDIR=${INCLUDEDIR}/tirpc

#.x.h:
#	${RPCCOM} -h -DWANT_NFS3 ${.IMPSRC} -o ${.TARGET}

include $(TOP)rules.mk

$(RPCHDRS): crypt.x
	$(RM) $(RPCHDRS)
	$(RPCGEN) -h -o crypt.h $<

