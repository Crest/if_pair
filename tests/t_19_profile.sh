#!/bin/sh
# DTrace profiling experiment for the remaining big-iron scaling
# hypothesis (NOTES.md: cross-CPU coordination cost).  Runs iperf3
# with LOAD_P parallel connections (default 64 - the collapse region
# on the 128-core server) and samples on-CPU kernel functions with
# the profile provider while the load runs, then buckets the samples:
#
#   locks    - spin/sleep lock primitives and lock_delay
#   sched    - scheduler, wakeups, sleepqueues, IPIs
#   copy     - data movement (bcopy/copyin/copyout/uiomove/...)
#   alloc    - UMA and mbuf lifecycle
#   proto    - TCP/IP/netisr/if_pair delivery work
#   other    - everything else
#
# If the coordination hypothesis holds on a large system, locks +
# sched dominate the non-idle profile at high connection counts; a
# copy-dominated profile would point at plain memory bandwidth
# instead.  The test asserts only that the load and the capture
# succeeded - the distribution is the result, printed for the
# operator, one function per line for the top consumers.
#
# Requires iperf3 and a working dtrace; skipped if either is missing.
# BENCH_SECS (default 15) is the iperf3 duration, PROF_SECS (default
# 10) the profiling window inside it, LOAD_P the connection count.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi
if ! command -v dtrace >/dev/null 2>&1; then
	log "SKIP: dtrace not installed"
	exit 0
fi
# End-to-end smoke of the capture pipeline before the real
# benchmark.  Two platform lessons are baked in: idle vCPUs park in
# the hypervisor and yield almost no kernel-PC samples, so kernel-
# bound load (dd zero->null) must run during the probe window; and
# func() can render blank keys here, so addresses are aggregated raw
# and formatted with %a at print time, which always yields a symbol
# or a hex fallback.
dd if=/dev/zero of=/dev/null bs=1m >/dev/null 2>&1 &
DD=$!
smoke=$(dtrace -qn 'profile-497 /arg0 != 0/ { @[arg0] = count(); }
    tick-1s { printa("%a %@d\n", @); exit(0); }' 2>&1)
rc=$?
kill "$DD" 2>/dev/null
if [ "$rc" -ne 0 ]; then
	log "SKIP: dtrace not functional:"
	log "  $(echo "$smoke" | head -1)"
	exit 0
fi
if ! echo "$smoke" | \
    awk 'NF == 2 && $2 ~ /^[0-9]+$/ { ok = 1 } END { exit(ok ? 0 : 1) }'
then
	echo "$smoke" | head -5 >&2
	log "SKIP: kernel-PC sampling not usable on this system"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-15}
PROF=${PROF_SECS:-10}
P=${LOAD_P:-64}

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" jexec "$J1" ping -q -o -c 3 -t 2 "$B4"

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
sleep 1

OUT=$(mktemp -t ifp_prof) || fail "mktemp failed"
cleanup_push "rm -f ${OUT} ${OUT}.err ${OUT}.sum"

log "iperf3 -P ${P} for ${SECS}s, profiling ${PROF}s inside the run"
jexec "$J2" timeout $((SECS + 30)) iperf3 -c "$A4" -P "$P" -t "$SECS" \
    >/dev/null 2>&1 &
CLI=$!
sleep 2

if ! dtrace -x aggsize=16m -qn "
	profile-497
	/arg0 != 0/
	{ @f[arg0] = count(); }
	tick-${PROF}s { printa(\"%a %@d\n\", @f); exit(0); }" \
    > "$OUT" 2> "${OUT}.err"; then
	head -3 "${OUT}.err" >&2
	fail "dtrace capture failed"
fi
log "ok: capture finished"

wait "$CLI" || fail "iperf3 did not complete under profiling load"
log "ok: iperf3 completed"

awk -v logpfx="" '
    NF == 2 && $2 ~ /^[0-9]+$/ {
	total += $2
	if ($1 ~ /idle|mwait|_wfi|cpu_search/) { idle += $2; next }
	live += $2
	sub(/\+0x[0-9a-fA-F]+$/, "", $1)	# per-PC -> per-function
	f[$1] += $2
	if ($1 ~ /lock|mtx|rmlock|rwlock|sxlock|turnstile|lock_delay|atomic/)
		b["locks"] += $2
	else if ($1 ~ /sched|tdq|ipi_|sleepq|wakeup/ ||
	    $1 ~ /setrunnable|choosethread|mi_switch|critical_exit/)
		b["sched"] += $2
	else if ($1 ~ /copy|bcopy|memcpy|bzero|memmove|uiomove/)
		b["copy"] += $2
	else if ($1 ~ /uma_|mb_|m_free|m_get|malloc|free/)
		b["alloc"] += $2
	else if ($1 ~ /tcp_|ip_|ip6_|udp_|in_|in6_|netisr/ ||
	    $1 ~ /pair_|sbappend|soreceive|sosend/)
		b["proto"] += $2
	else
		b["other"] += $2
    }
    END {
	if (live == 0) { print "NOSAMPLES"; exit }
	printf "samples: %d total, %d non-idle (%.1f%% idle)\n",
	    total, live, idle * 100.0 / (total ? total : 1)
	n = split("locks sched copy alloc proto other", names, " ")
	for (i = 1; i <= n; i++)
		printf "bucket %-6s %6.1f%%\n",
		    names[i], b[names[i]] * 100.0 / live
	# top functions, simple selection sort of the top 12
	for (r = 1; r <= 12; r++) {
		best = ""; bc = -1
		for (fn in f)
			if (f[fn] > bc) { bc = f[fn]; best = fn }
		if (best == "") break
		printf "top %2d  %-40s %6.1f%%\n",
		    r, best, bc * 100.0 / live
		delete f[best]
	}
    }' "$OUT" > "${OUT}.sum"

if grep -q NOSAMPLES "${OUT}.sum"; then
	log "raw capture head for diagnosis:"
	head -5 "$OUT" | while IFS= read -r line; do log "  |$line|"; done
	head -3 "${OUT}.err" | while IFS= read -r line; do log "  !$line!"; done
	fail "dtrace captured no parseable samples"
fi
while IFS= read -r line; do
	log "$line"
done < "${OUT}.sum"
pass
