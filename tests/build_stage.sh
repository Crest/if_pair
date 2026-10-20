#!/bin/sh
# build_stage.sh - build the kwq and if_pair modules of one commit into a
# directory, for tests/compare.sh.
#
#   build_stage.sh COMMIT OUTDIR            build here
#   build_stage.sh -r HOST COMMIT OUTDIR    build on HOST (OUTDIR is there)
#
# The commit's kwq/kwq, if_pair.c, Makefile, if_pair.4 and extras/if_pair_tq
# are exported with git-archive into a scratch directory and built with
# make; MAKEARGS (e.g. "SYSDIR=/usr/src/sys KERNBUILDDIR=/usr/obj/..." or
# "KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/GENERIC") is passed to
# every make.  On a remote host the scratch directory is OUTDIR/src and
# make runs there, so the host needs only make and the kernel sources or
# objects, not git.  Results: OUTDIR/kwq.ko, OUTDIR/if_pair.ko and, where
# the commit has it, OUTDIR/if_pair_tq.ko.
set -e
HOST=""
if [ "$1" = -r ]; then HOST=$2; shift 2; fi
COMMIT=$1; OUTDIR=$2
[ -n "$COMMIT" ] && [ -n "$OUTDIR" ] || { echo "usage: build_stage.sh [-r HOST] COMMIT OUTDIR" >&2; exit 2; }
cd "$(dirname "$0")/.."
PATHS="kwq/kwq if_pair.c Makefile if_pair.4"
git cat-file -e "$COMMIT:extras/if_pair_tq/if_pair_tq.c" 2>/dev/null && PATHS="$PATHS extras/if_pair_tq"
# shellcheck disable=SC2086
BUILD='set -e; cd "$1"; mkdir -p src; tar -xf - -C src
cd src/kwq/kwq && make -s $MAKEARGS >/dev/null && cp kwq.ko "$1"
cd ../.. && make -s $MAKEARGS if_pair.ko >/dev/null && cp if_pair.ko "$1"
if [ -d extras/if_pair_tq ]; then cd extras/if_pair_tq && make -s $MAKEARGS >/dev/null && cp if_pair_tq.ko "$1"; fi
cd "$1" && ls -l *.ko'
if [ -n "$HOST" ]; then
	# shellcheck disable=SC2029
	git archive --format=tar "$COMMIT" $PATHS | ssh "$HOST" "mkdir -p '$OUTDIR' && MAKEARGS='${MAKEARGS:-}' sh -c '$BUILD' build_stage '$OUTDIR'"
else
	mkdir -p "$OUTDIR"
	git archive --format=tar "$COMMIT" $PATHS | MAKEARGS="${MAKEARGS:-}" sh -c "$BUILD" build_stage "$OUTDIR"
fi
