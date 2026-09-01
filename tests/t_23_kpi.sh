#!/bin/sh
# tcp_tso_chop() KPI correctness via tso_wrap (HANDOFF.txt Step A,
# CHOPPER.txt P2/P6a).  Inverts t_22's deliver-whole assertion:
# the pair runs at mtu 1500 with TSO on both sides, and tso_wrap
# is attached to the SENDING side's if_output - so every TSO
# chain must be chopped back to wire-size segments by the KPI
# before the pair carries it.  The sending side stays ON THE HOST
# (host -> jail traffic): tso_wrap's control sysctl is global and
# resolves interfaces in the caller's vnet, so the wrapped side
# must live in the host vnet.  Asserts:
#
#   1. Baseline (wrapper not attached): average tx packet size
#      far exceeds the MTU (deliver-whole works, as t_22 proved).
#   2. Wrapper attached: average tx packet size <= MTU + slop -
#      the chopper really segments - while net.tso_wrap.chopped
#      and .segments advance and the transfer completes (which
#      also validates per-segment checksums end to end: the
#      receiving jail's TCP accepts every segment).
#
# Throughput is logged, not asserted.  Requires iperf3 and
# tso_wrap.ko (path via TSO_WRAP_KO, default next to the test
# tree in extras/); skipped when missing.  BENCH_SECS (default
# 5) per run, LOAD_P (default 4) connections.
. "$(dirname "$0")/lib.sh"

TSO_WRAP_KO=${TSO_WRAP_KO:-"$(dirname "$0")/../extras/tso_wrap/tso_wrap.ko"}

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi
if ! kldstat -q -m tso_wrap; then
	if [ ! -f "$TSO_WRAP_KO" ]; then
		log "SKIP: tso_wrap.ko not found (set TSO_WRAP_KO)"
		exit 0
	fi
fi

test_init

if ! kldstat -q -m tso_wrap; then
	must "load tso_wrap" kldload "$TSO_WRAP_KO"
	cleanup_push "kldunload tso_wrap"
fi

SECS=${BENCH_SECS:-5}
P=${LOAD_P:-4}

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1)
create_pair
mkjail "$J1"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "mtu 1500 side a" jexec "$J1" ifconfig "$PAIRA" mtu 1500
must "mtu 1500 side b" ifconfig "$PAIRB" mtu 1500
must "tso side a" jexec "$J1" ifconfig "$PAIRA" tso
must "tso side b" ifconfig "$PAIRB" tso
must "address side a" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "address side b" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" ping -q -o -c 3 -t 2 "$A4"

# Host-side tx counters of the pair's b side.
ostats() {
	netstat --libxo json -bnI "$1" | tr ',{}' '\n\n\n' | awk -F: '
	    /"sent-packets"/ && !p { p = $2 }
	    /"sent-bytes"/ && !b { b = $2 }
	    END { print p+0, b+0 }'
}

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
must_retry 10 "iperf3 server listening" nc -z -w 1 "$A4" 5201

# run_avg label: one bulk run from the HOST; RUN_AVG = avg tx size.
run_avg() {
	# Save the label: the "set --" below clobbers $1.
	label=$1
	set -- $(ostats "$PAIRB"); p0=$1; b0=$2
	out=$(timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
	    -P "$P" -t "$SECS" 2>&1) || fail "iperf3 run ($label) failed"
	rate=$(echo "$out" | awk '
	    /receiver/ {
		for (i = 1; i <= NF; i++)
			if ($i == "Gbits/sec") {
				r = $(i - 1)
				if ($0 ~ /^\[SUM\]/)
					s = r
			}
	    }
	    END { print (s != "" ? s : r) }')
	set -- $(ostats "$PAIRB"); p1=$1; b1=$2
	RUN_AVG=$(( (b1 - b0) / (p1 - p0 + 1) ))
	log "$label: ${rate:-?} Gbit/s, avg tx packet ${RUN_AVG} bytes"
}

run_avg "deliver-whole baseline"
base_avg=$RUN_AVG
[ "$base_avg" -gt 3000 ] || \
    fail "baseline avg ${base_avg}: TSO chains are not crossing whole"

# Attach the wrapper to the host-side sending interface.
must "attach tso_wrap to ${PAIRB}" \
    sysctl net.tso_wrap.control="$PAIRB"
cleanup_push "sysctl net.tso_wrap.control=-${PAIRB}"

c0=$(sysctl -n net.tso_wrap.chopped)
s0=$(sysctl -n net.tso_wrap.segments)
run_avg "tso_wrap chopping    "
wrap_avg=$RUN_AVG
c1=$(sysctl -n net.tso_wrap.chopped)
s1=$(sysctl -n net.tso_wrap.segments)

log "tso_wrap counters: chopped +$((c1 - c0)), segments +$((s1 - s0))"
[ "$((c1 - c0))" -gt 0 ] || fail "wrapper attached but chopped nothing"
[ "$wrap_avg" -le 1600 ] || \
    fail "avg ${wrap_avg} with wrapper attached: KPI did not segment"
log "ok: KPI chops to wire size (${base_avg} -> ${wrap_avg} bytes/packet)"
pass
