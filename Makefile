# Out-of-tree kernel module for FreeBSD 15.0+.
# Build on FreeBSD with:  make
# If your kernel sources are not in /usr/src/sys:  make SYSDIR=/path/to/src/sys

KMOD=	if_pair
SRCS=	if_pair.c
SRCS+=	opt_inet.h opt_inet6.h

.include <bsd.kmod.mk>
