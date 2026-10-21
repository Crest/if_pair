# schedprobe.ko - the cost of the scheduler operations a kwq worker would use
# to share its CPU.  Standalone: nothing here depends on kwq.  Build like any
# kernel module:
#   make [SYSDIR=/path/to/src/sys] [KERNBUILDDIR=/path/to/kernel/objects]
# Run:  kldload ./schedprobe.ko; sysctl kern.schedprobe.run=1; dmesg | grep schedprobe

KMOD=	schedprobe
SRCS=	schedprobe.c

.include <bsd.kmod.mk>
