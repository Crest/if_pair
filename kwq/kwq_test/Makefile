# kwq_test.ko - synthetic kwq clients driven by sysctl.  Depends on kwq.ko.

KMOD=	kwq_test
SRCS=	kwq_test.c
CFLAGS+=	-I${.CURDIR}/../kwq

uninstall:
	rm -f ${DESTDIR}${KMODDIR}/${KMOD}.ko
	${KLDXREF_CMD} ${DESTDIR}${KMODDIR}

.include <bsd.kmod.mk>
