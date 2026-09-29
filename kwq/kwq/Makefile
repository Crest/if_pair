# kwq.ko - kernel work queues (out-of-tree module, FreeBSD 15.x).
# Build with `make`; point SYSDIR at the kernel sources if they are not
# in /usr/src/sys.  Standalone kmod builds pick up GENERIC's option set
# (VIMAGE, INET, INET6, ...) from sys/conf/config.mk; to build against a
# debug kernel's options (INVARIANTS, WITNESS) pass
# KERNBUILDDIR=/usr/obj/.../sys/GENERIC-DEBUG so its opt_global.h is used.

KMOD=	kwq
SRCS=	kwq.c kwq_worker.c kwq_sched.c kwq_sdt.c kwq_ddb.c
SRCS+=	kwq.h kwq_internal.h kwq_sched.h kwq_sched_env.h kwq_sdt.h
SRCS+=	opt_ddb.h

# Standalone builds (no KERNBUILDDIR) take GENERIC's options from
# kmod.opts.mk, which leaves out KDTRACE_HOOKS; the stock kernel has it,
# and without it the kwq provider (kwq_sdt.c) compiles to nothing.
KERN_OPTS+=	KDTRACE_HOOKS
# DWARF for ctfconvert: the kwq.d translator needs struct kwq's CTF (built
# WITH_CTF=1).  DEBUG_FLAGS is the only way kmod.mk puts -g in CFLAGS, and
# it also makes "make install" ship kwq.ko.debug into
# /usr/lib/debug/boot/modules, which exists only after a full installkernel;
# the CTF survives the strip, so install the module alone unless asked.
DEBUG_FLAGS?=	-g
.if !defined(KWQ_INSTALL_DEBUG)
INSTALL_NODEBUG=
.endif
# make KWQ_NO_SDT=1: probe sites compiled out, to measure the disabled-probe cost
.if defined(KWQ_NO_SDT)
CFLAGS+=	-DKWQ_NO_SDT
.endif
# kwq_sched.h includes <kwq_sched_env.h>; the kernel one lives here.
CFLAGS+=	-I${.CURDIR}

# kmod.mk localises every symbol unless told otherwise; export the public
# KPI (kwq.h) and nothing else.
EXPORT_SYMS=	kwq_create kwq_activate kwq_drain kwq_destroy \
		kwq_enqueue kwq_enqueue_list kwq_cpu_for_hash kwq_scatter \
		kwq_notifier_init kwq_notify kwq_notify_cancel \
		kwq_requeue kwq_budget_left kwq_name kwq_class

.include <bsd.kmod.mk>
