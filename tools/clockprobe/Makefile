# clockprobe.ko - characterise the CPU clocks of this machine.  Standalone:
# nothing here depends on kwq.  Build like any kernel module:
#   make [SYSDIR=/path/to/src/sys] [KERNBUILDDIR=/path/to/kernel/objects]
# Run:  kldload ./clockprobe.ko; sysctl kern.clockprobe.run=1; dmesg | grep clockprobe

KMOD=	clockprobe
SRCS=	clockprobe.c

.include <bsd.kmod.mk>
