# geom_kwqeli.so: the userland half of geli(8) (releng/15.1 lib/geom/eli)
# for class KWQELI, built against this directory's copy of g_eli.h.  Not
# installed: geom(8) finds it through GEOM_LIBRARY_PATH,
#   env GEOM_LIBRARY_PATH=$(make -V .OBJDIR) geom kwqeli onetime md0
# SRCTOP is a FreeBSD source tree of the running release, for geom(8)'s
# own headers and subr.c.

SRCTOP?=	/usr/src
SHLIB_NAME=	geom_kwqeli.so
SRCS=	geom_eli.c subr.c
SRCS+=	g_eli_crypto.c g_eli_hmac.c g_eli_key.c pkcs5v2.c
.PATH:	${.CURDIR}/../sys ${SRCTOP}/sbin/geom/misc
CFLAGS+=	-I${.CURDIR}/../sys -I${SRCTOP}/sbin/geom -I${SRCTOP}/sys
LDADD=	-lmd -lcrypto
# as the base build (WARNS 6 with -Wno-pointer-sign); upstream code
CFLAGS+=	-Wno-pointer-sign
MAN=
# libgeom's gctl_* and geom(8)'s own symbols come from the geom binary.
NO_WMISSING_VARIABLE_DECLARATIONS=

.include <bsd.lib.mk>
