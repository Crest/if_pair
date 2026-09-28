# kwq.ko - kernel work queues (out-of-tree module, FreeBSD 15.x).
# Build with `make`; point SYSDIR at the kernel sources if they are not
# in /usr/src/sys.  Standalone kmod builds pick up GENERIC's option set
# (VIMAGE, INET, INET6, ...) from sys/conf/config.mk; to build against a
# debug kernel's options (INVARIANTS, WITNESS) pass
# KERNBUILDDIR=/usr/obj/.../sys/GENERIC-DEBUG so its opt_global.h is used.

KMOD=	kwq
SRCS=	kwq.c kwq_worker.c kwq_sched.c
SRCS+=	kwq.h kwq_internal.h kwq_sched.h kwq_sched_env.h
# kwq_sched.h includes <kwq_sched_env.h>; the kernel one lives here.
CFLAGS+=	-I${.CURDIR}

# kmod.mk localises every symbol unless told otherwise; export the public
# KPI (kwq.h) and nothing else.
EXPORT_SYMS=	kwq_create kwq_activate kwq_drain kwq_destroy \
		kwq_enqueue kwq_enqueue_list kwq_cpu_for_hash kwq_scatter \
		kwq_notifier_init kwq_notify kwq_notify_cancel \
		kwq_requeue kwq_budget_left kwq_name kwq_class

.include <bsd.kmod.mk>
