# The taskqueue-based if_pair, frozen as a benchmark baseline.
# Loads next to if_pair.ko: module if_pair_tq, interfaces pairtq<N>a/b,
# sysctls under net.link.pairtq.  Build like if_pair:
#   make            (kernel sources in /usr/src)
#   kldload ./if_pair_tq.ko
KMOD=	if_pair_tq
SRCS=	if_pair_tq.c
SRCS+=	opt_inet.h opt_inet6.h

.include <bsd.kmod.mk>
