# placesim: BULK placement simulator (../../SPEC-p6g.md S3); links the
# kernel's kwq_place.c.  `make test' runs the unit tests and the
# validation suite (V1-V4).

CFLAGS+=	-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I. -I../../kwq
LDFLAGS+=	-lm
SHARED=		../../kwq/kwq_place.c ../../kwq/kwq_place.h kwq_place_env.h

all: placesim kp_test pp

placesim: placesim.c ${SHARED}
	${CC} ${CFLAGS} -Wno-unused-function -o placesim placesim.c ../../kwq/kwq_place.c ${LDFLAGS}

kp_test: kp_test.c ${SHARED}
	${CC} ${CFLAGS} -o kp_test kp_test.c ../../kwq/kwq_place.c

pp: pp.c
	${CC} -O2 -Wall -o pp pp.c -lpthread

test: placesim kp_test
	./kp_test
	sh ./suite.sh

clean:
	rm -f placesim kp_test pp *.o

.PHONY: all test clean
