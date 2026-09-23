# Reproducers for the devel/libdispatch FreeBSD port bugs described in
# BUGREPORT.md.  "make run" builds everything and runs the whole matrix
# against the installed library; DISPATCH_LIB=/path/to/build runs it
# against a rebuilt libdispatch.so instead (see run.sh).

DISPATCH_PROGS=	repro_min repro_tid_alias repro_tid_ctl repro_once repro_group repro_runloop
STANDALONE=	repro_workq_loop
PROGS=		${DISPATCH_PROGS} ${STANDALONE}

CC?=		cc
CFLAGS+=	-O1 -Wall -I/usr/local/include
DISPATCH_LDFLAGS=	-fuse-ld=lld -L/usr/local/lib -ldispatch -lpthread

all: ${PROGS}

.for p in ${DISPATCH_PROGS}
${p}: ${p}.c
	${CC} ${CFLAGS} -o ${.TARGET} ${.ALLSRC} ${DISPATCH_LDFLAGS}
.endfor

${STANDALONE}: ${STANDALONE}.c
	${CC} ${CFLAGS} -o ${.TARGET} ${.ALLSRC}

run: all
	sh ${.CURDIR}/run.sh

clean:
	rm -f ${PROGS} *.core
