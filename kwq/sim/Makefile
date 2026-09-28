# kwq scheduler simulator: links the kernel's kwq_sched.c against mock
# clocks and threads.  `make test` runs the suite (SCHED.md S10, PLAN P1a).

PROG=	kwqsim
SRCS=	sim.c ../kwq/kwq_sched.c
CFLAGS+=	-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -I../kwq
LDFLAGS+=	-lm

all: ${PROG}

${PROG}: ${SRCS} kwq_sched_env.h ../kwq/kwq_sched.h
	${CC} ${CFLAGS} -o ${PROG} ${SRCS} ${LDFLAGS}

test: ${PROG}
	./${PROG} suite

clean:
	rm -f ${PROG} *.o *.csv suite.out

.PHONY: all test clean
