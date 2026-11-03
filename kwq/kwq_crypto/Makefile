# kwq_crypto.ko - a dummy BULK client benchmarking AES-XTS through crypto(9)
# the way GELI does (../PLAN.txt P6d).  Depends on kwq.ko and crypto(4).

KMOD=	kwq_crypto
SRCS=	kwq_crypto.c
SRCS+=	bus_if.h device_if.h cryptodev_if.h
CFLAGS+=	-I${.CURDIR}/../kwq

uninstall:
	rm -f ${DESTDIR}${KMODDIR}/${KMOD}.ko
	${KLDXREF_CMD} ${DESTDIR}${KMODDIR}

.include <bsd.kmod.mk>
