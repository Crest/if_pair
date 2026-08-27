# Prototype pfil module; see lro2tso.c.  Build like if_pair:
#   make            (kernel sources in /usr/src)
#   kldload ./lro2tso.ko
KMOD=	lro2tso
SRCS=	lro2tso.c
SRCS+=	opt_inet.h opt_inet6.h

.include <bsd.kmod.mk>
