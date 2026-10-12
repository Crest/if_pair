#!/bin/sh
# Round-trip latency through one pair between two jails: a ping flood
# (as fast as the replies come: the dense case) and a 1 kHz ping (the
# sparse case), each for COUNT packets; with a kwq that has the idle poll
# (the kwq-spin-experiment branch) once per value of
# kern.kwq.net.idle_spin_us in SPINS (default "0 50 100"; the original is
# restored), otherwise once, as the baseline driver runs.  Per run one line: min, avg,
# max and standard deviation of the round trip in microseconds, the
# packet loss (should be 0; a flood that loses packets is throttled by
# ping to 100 packets per second, so a lossy run is a long one), and for
# kwq the worker wakeups and idle polls per packet summed over all CPUs,
# which shows whether the poll engaged (dense) or stayed out (sparse,
# SCHED.md S16 rule 2).  A measurement harness: it fails only if a run
# does not complete.  PAIR=pairtq PAIR_MOD=if_pair_tq runs the taskqueue
# baseline (lib.sh).
. "$(dirname "$0")/lib.sh"

test_init

COUNT=${COUNT:-20000}
SPARSE=${SPARSE:-3000}
SPINS=${SPINS:-"0 50 100"}
has_oid() { sysctl -n "$1" >/dev/null 2>&1; }
HAVE_SPIN=0
[ "$PAIR" = pair ] && has_oid kern.kwq.net.idle_spin_us && HAVE_SPIN=1
[ "$HAVE_SPIN" = 1 ] || SPINS="-"

create_pair
JS=$(jname s1); JC=$(jname c1)
mkjail "$JS"; mkjail "$JC"
must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet 10.99.1.1/32 10.99.1.2 up && jexec $JC ifconfig $PAIRB inet 10.99.1.2/32 10.99.1.1 up"
must_retry 5 "reachable" jexec "$JC" ping -c 1 -t 1 10.99.1.1
# The responder's ICMP rate limit (net.inet.icmp.icmplim, 200/s) would
# throttle both runs to a third and a fifth of their rate; it is a per-vnet
# knob, so lifting it inside the server jail leaves the host untouched.
must "icmplim in $JS" jexec "$JS" sysctl -q net.inet.icmp.icmplim=1000000

if [ "$HAVE_SPIN" = 1 ]; then
	orig_spin=$(sysctl -n kern.kwq.net.idle_spin_us)
	cleanup_push "sysctl -q kern.kwq.net.idle_spin_us=$orig_spin"
	if [ -n "$KWQ_HW" ]; then	# the poll's hardware wait, kern.kwq.poll_hw
		orig_hw=$(sysctl -n kern.kwq.poll_hw)
		cleanup_push "sysctl -q kern.kwq.poll_hw=$orig_hw"
		must "poll_hw=$KWQ_HW" sysctl -q kern.kwq.poll_hw="$KWQ_HW"
	fi
	log "poll: $(sysctl -n kern.kwq.poll_mode), period $(sysctl -n kern.kwq.poll_period_ns) ns"
fi

kwq_sum() {	# wakeups spins spin_hits, summed over the CPUs
	[ "$PAIR" = pair ] || { echo "0 0 0"; return; }
	sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "wakeups" { w += $NF } $6 == "spins" { s += $NF } $6 == "spin_hits" { h += $NF } END { print w + 0, s + 0, h + 0 }'
}

rtt_line() {	# "min avg max stddev loss%" from ping's summary, times in us
	awk -F'[ /]' '/packets received/ { for (i = 1; i <= NF; i++) if ($i ~ /%$/) loss = $i }
	    /round-trip/ { for (i = 1; i <= NF; i++) if ($i == "=") { printf("%.0f %.0f %.0f %.0f %s", $(i + 1) * 1000, $(i + 2) * 1000, $(i + 3) * 1000, $(i + 4) * 1000, loss); exit } }' "$1"
}

failed=0
run() {	# label count ping-options...
	label=$1; n=$2; shift 2; opts="$*"
	set -- $(kwq_sum); w0=$1; s0=$2; h0=$3
	jexec "$JC" ping -q -c "$n" $opts 10.99.1.1 > "$TMPD/ping.out" 2>&1
	set -- $(kwq_sum); w1=$1; s1=$2; h1=$3
	rtt=$(rtt_line "$TMPD/ping.out")
	if [ -z "$rtt" ]; then
		log "$label: FAILED ($(tail -1 "$TMPD/ping.out"))"; failed=$((failed + 1)); return
	fi
	set -- $rtt
	k=""
	[ "$PAIR" = pair ] && k=$(echo "$w0 $w1 $s0 $s1 $h0 $h1 $n $HAVE_SPIN" | awk '{ printf("; kwq per packet: wakeups %.2f", ($2 - $1) / $7); if ($8 == 1) printf(", polls %.2f (hit %.0f%%)", ($4 - $3) / $7, ($4 - $3) > 0 ? ($6 - $5) * 100 / ($4 - $3) : 0) }')
	log "$label: rtt us min $1 avg $2 max $3 stddev $4, loss $5$k"
}

TMPD=$(mktemp -d /tmp/${TEST}.XXXXXX); cleanup_push "rm -rf $TMPD"
for sp in $SPINS; do
	tag="$PAIR"
	if [ "$sp" != "-" ]; then
		must "idle_spin_us=$sp" sysctl -q kern.kwq.net.idle_spin_us="$sp"
		tag="idle_spin_us=$sp"
	fi
	run "$tag flood ($COUNT)" "$COUNT" -f
	run "$tag 1 kHz ($SPARSE)" "$SPARSE" -i 0.001
done

[ "$failed" -gt 0 ] && fail "$failed runs did not complete"
pass
