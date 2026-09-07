#!/bin/sh
# if_pair offload matrix: the exact grid of t_25_epair_tso.sh run
# against if_pair(4), for the side-by-side driver comparison.
#
#   mtu {1500, 16384} x offloads {none, txcsum, txcsum+tso}
#     x conns PAIR_CONNS (default "1 4 16")
#
# One row per cell: throughput and average tx packet size
# (sent-bytes / sent-packets delta on the sending jail's side; note
# if_pair is bare L3, so wire-size rows read ~1499, not epair's
# ether-framed ~1513).  "-txcsum tso" is unsupported by design on
# if_pair exactly as on epair (the TXCSUM<->TSO coupling revokes
# the grant), so the matrix has three offload rows.  Unlike epair
# there is NO peer sync - if_pair's sides are independent - so the
# offload flags are set explicitly on both sides for every cell.
# Capability semantics themselves are t_10 (txcsum) and t_22
# (tso); this test only asserts per cell that the run completes,
# TSO rows exceed the mtu (deliver-whole) and non-TSO rows stay at
# wire size.  SKIPs when the pair does not advertise TSO4.
# BENCH_SECS (default 5) per cell.
. "$(dirname "$0")/lib.sh"

if ! command -v iperf3 >/dev/null 2>&1; then
	log "SKIP: iperf3 not installed"
	exit 0
fi

test_init

SECS=${BENCH_SECS:-5}
CONNS=${PAIR_CONNS:-"1 4 16"}

A4=192.0.2.1; B4=192.0.2.2
J1=$(jname 1); J2=$(jname 2)
create_pair
mkjail "$J1"
mkjail "$J2"
must "move ${PAIRA} into ${J1}" ifconfig "$PAIRA" vnet "$J1"
must "move ${PAIRB} into ${J2}" ifconfig "$PAIRB" vnet "$J2"

case "$(jexec "$J1" ifconfig -m "$PAIRA" | \
    awk -F'[<>]' '/capabilities=/ { print $2; exit }')" in
*TSO4*) ;;
*)	log "SKIP: ${PAIRA} does not advertise TSO4"
	exit 0 ;;
esac

must "jail1 side" jexec "$J1" ifconfig "$PAIRA" inet "${A4}/32" "$B4" up
must "jail2 side" jexec "$J2" ifconfig "$PAIRB" inet "${B4}/32" "$A4" up
must_retry 5 "baseline ping" jexec "$J2" ping -q -o -c 3 -t 2 "$A4"

# jail2-side tx counters of its pair end.
ostats() {
	jexec "$J2" netstat --libxo json -bnI "$PAIRB" | tr ',{}' '\n\n\n' | \
	    awk -F: '
	    /"sent-packets"/ && !p { p = $2 }
	    /"sent-bytes"/ && !b { b = $2 }
	    END { print p+0, b+0 }'
}

jexec "$J1" iperf3 -s >/dev/null 2>&1 &
SRV=$!
cleanup_push "kill ${SRV}"
must_retry 10 "iperf3 server listening" jexec "$J2" nc -z -w 1 "$A4" 5201

# setcaps iface-flags...: apply the same offload flags to both sides
# (no peer sync on if_pair - explicit on each).
setcaps() {
	jexec "$J1" ifconfig "$PAIRA" "$@" || fail "setcaps ${PAIRA} $*"
	jexec "$J2" ifconfig "$PAIRB" "$@" || fail "setcaps ${PAIRB} $*"
}

# One matrix cell; logs "mtu offloads conns rate avg".
run_cell() {
	mtu=$1; label=$2; conns=$3
	set -- $(ostats); p0=$1; b0=$2
	out=$(jexec "$J2" timeout $((SECS + 30)) iperf3 -f g -c "$A4" \
	    -P "$conns" -t "$SECS" 2>&1) || \
	    fail "iperf3 failed (mtu ${mtu} ${label} P=${conns})"
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
	set -- $(ostats); p1=$1; b1=$2
	avg=$(( (b1 - b0) / (p1 - p0 + 1) ))
	log "$(printf '%5s  %-11s %4s  %8s  %6s' \
	    "$mtu" "$label" "$conns" "${rate:-?}" "$avg")"
	case "$label" in
	*tso*)	[ "$avg" -gt "$mtu" ] || \
		    fail "avg ${avg} <= mtu ${mtu} with TSO: not deliver-whole" ;;
	*)	[ "$avg" -le $((mtu + 100)) ] || \
		    fail "avg ${avg} > mtu ${mtu} without TSO" ;;
	esac
}

log "$(printf '%5s  %-11s %4s  %8s  %6s' \
    mtu offloads conns Gbit/s avg-tx)"
for mtu in 1500 16384; do
	jexec "$J1" ifconfig "$PAIRA" mtu "$mtu" || fail "mtu ${PAIRA}"
	jexec "$J2" ifconfig "$PAIRB" mtu "$mtu" || fail "mtu ${PAIRB}"
	for combo in "-txcsum,-tso:none" "txcsum,-tso:txcsum" \
	    "txcsum,tso:txcsum+tso"; do
		flags=$(echo "${combo%%:*}" | tr ',' ' ')
		label=${combo##*:}
		# shellcheck disable=SC2086
		setcaps $flags
		for p in $CONNS; do
			run_cell "$mtu" "$label" "$p"
		done
	done
done

log "ok: full offload matrix complete (TSO deliver-whole holds)"
pass
