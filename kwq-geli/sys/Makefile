# geom_kwqeli.ko: GEOM_ELI of releng/15.1 (sys/geom/eli, copied into this
# directory) as class KWQELI, loadable beside geom_eli.ko (../README.md).
# Build with `make`; SYSDIR and KERNBUILDDIR as for ../../kwq.

KMOD=	geom_kwqeli
SRCS=	g_eli.c
SRCS+=	g_eli_crypto.c
SRCS+=	g_eli_ctl.c
SRCS+=	g_eli_hmac.c
SRCS+=	g_eli_integrity.c
SRCS+=	g_eli_key.c
SRCS+=	g_eli_key_cache.c
SRCS+=	g_eli_privacy.c
SRCS+=	pkcs5v2.c
SRCS+=	vnode_if.h
# kwq.h, the kwq(9) KPI the port uses
CFLAGS+=	-I${.CURDIR}/../../kwq/kwq

.include <bsd.kmod.mk>
