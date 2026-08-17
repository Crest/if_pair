# Out-of-tree kernel module for FreeBSD 15.0+.
# Build on FreeBSD with:  make
# If your kernel sources are not in /usr/src/sys:  make SYSDIR=/path/to/src/sys
#
# `make install` installs if_pair.ko into KMODDIR (default
# /boot/modules) and the manual page under the local base prefix
# (where a port would put it), so `man if_pair` works after
# installation.  Before installing, use `man ./if_pair.4`.

KMOD=	if_pair
SRCS=	if_pair.c
SRCS+=	opt_inet.h opt_inet6.h

# bsd.kmod.mk has no manual page handling of its own; reuse
# bsd.man.mk (MAN, maninstall, MK_MANCOMPRESS compression) and point
# MANDIR at the local base prefix.  bsd.man.mk appends the section
# number to MANDIR.
LOCALBASE?=	/usr/local
MANDIR=		${LOCALBASE}/share/man/man
MAN=		if_pair.4
MLINKS=		if_pair.4 pair.4

.include <bsd.kmod.mk>
.include <bsd.man.mk>

# bsd.prog.mk normally hangs man page building off `all`; kmod.mk
# does not, so wire it up ourselves.
all: all-man

# The man4 directory may not exist yet on a system without ports.
# (kmod.mk only .ORDERs beforeinstall, it never invokes it, so the
# directory creation hangs off afterinstall alongside maninstall.)
afterinstall: _man4dir maninstall
.ORDER: _man4dir maninstall
_man4dir: .PHONY
	${INSTALL} -d ${DESTDIR}${MANDIR}4
