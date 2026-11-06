# gelisim: kwq-geli on many CPUs, on the real scheduler core with
# keepturn.patch.  Userspace only.
PROG=	gelisim
CFLAGS+=	-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -Ibuild
LDFLAGS+=	-lm

all: ${PROG}

build/kwq_sched.c build/kwq_sched.h: keepturn.patch ../../kwq/kwq_sched.c ../../kwq/kwq_sched.h
	mkdir -p build
	cp ../../kwq/kwq_sched.c ../../kwq/kwq_sched.h build/
	cd build && patch -s -p1 < ../keepturn.patch

${PROG}: gelisim.c build/kwq_sched.c build/kwq_sched.h kwq_sched_env.h
	${CC} ${CFLAGS} -o ${PROG} gelisim.c build/kwq_sched.c ${LDFLAGS}

clean:
	rm -rf build ${PROG}

.PHONY: all clean
