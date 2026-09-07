KMOD=	tso_wrap
SRCS=	tso_wrap.c tcp_tso.c
SRCS+=	opt_inet.h opt_inet6.h

.PATH:	${.CURDIR}/../tcp_tso
CFLAGS+= -I${.CURDIR}/../tcp_tso

.include <bsd.kmod.mk>
