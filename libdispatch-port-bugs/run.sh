#!/bin/sh
#
# Run every reproducer and print one verdict line per case.
#
#   sh run.sh                              against the installed libdispatch
#   DISPATCH_LIB=/path/to/build sh run.sh  against a rebuilt libdispatch.so
#
# On the shipped 6.1.1_1 port the issue 1, 2 and 4 cases FAIL and the
# control PASSes; with libdispatch-freebsd-lock-fixes.diff applied every
# case PASSes.  The issue 3 harness has no pass/fail: it prints the count
# the port's loop produces next to the count the corrected loop produces.

cd "$(dirname "$0")" || exit 1
if [ -n "$DISPATCH_LIB" ]; then
	export LD_LIBRARY_PATH="$DISPATCH_LIB"
fi

TIMEOUT=60
LIB=$(ldd ./repro_min 2>/dev/null | awk '/libdispatch/ { print $3 }')

# Exit status -> verdict.  132 is SIGILL: libdispatch's DISPATCH_CLIENT_CRASH
# ends in __builtin_trap().
verdict() {
	case "$1" in
	0)	echo "PASS" ;;
	132)	echo "FAIL (SIGILL: DISPATCH_CLIENT_CRASH trap)" ;;
	124)	echo "FAIL (timeout)" ;;
	*)	echo "FAIL (exit $1)" ;;
	esac
}

# Print a case's label and verdict; on failure, also print the program's
# diagnostic output (minus the per-thread ID banner lines), indented.
report() {	# report <label> <status> <output>
	printf '%-52s %s\n' "$1" "$(verdict "$2")"
	if [ "$2" -ne 0 ]; then
		echo "$3" | grep -v '^thread [0-9]*: tid\|^worker tid\|^$' |
		    head -4 | sed 's/^/    /'
	fi
}

# Run one reproducer under a timeout and report it.
run() {		# run <label> <command...>
	label=$1
	shift
	out=$(timeout "$TIMEOUT" "$@" 2>&1)
	report "$label" $? "$out"
}

echo "libdispatch: $LIB"
echo

echo "== Issue 1: thread-ID aliasing in the lock owner encoding"
run "repro_min       (2 aliased threads, dispatch_sync)"	./repro_min
run "repro_tid_alias (8 threads x 20000 dispatch_sync)"	./repro_tid_alias
run "repro_once      (4 threads, contended dispatch_once)"	./repro_once
run "repro_tid_ctl   (control: distinct ID blocks)"		./repro_tid_ctl
echo

echo "== Issue 2: undefined _dispatch_ulock_wait / no-op wake"
run "repro_group     (dispatch_group_wait)"			./repro_group

out=$(LD_BIND_NOW=1 timeout "$TIMEOUT" ./repro_tid_ctl 2>&1)
report "LD_BIND_NOW=1   (immediate binding, any program)" $? "$out"

undefined=$(nm -D --undefined-only "$LIB" 2>/dev/null | grep '_dispatch')
printf '%-52s ' "nm              (undefined _dispatch_* symbols)"
if [ -z "$undefined" ]; then
	echo "PASS (none)"
else
	echo "FAIL"
	echo "$undefined" | sed 's/^ *U /    /'
fi
echo

echo "== Issue 4: main-queue run-loop handle truncated to the pipe's write end"
run "repro_runloop   (4CF handle: read end readable?)"	./repro_runloop
echo

echo "== Issue 3: workqueue monitor loop (standalone harness, no libdispatch)"
./repro_workq_loop | sed 's/^/    /'
