#!/bin/sh
# Interactive use beside a saturated NET worker (PLAN P4c).  One pair
# between two jails carries an iperf3 run of CONNS connections (default
# 1) for SECS seconds (default 10); the worker CPU W that the flow lands
# on is found during the lead-in and both iperf3 processes are moved off
# it, so that W holds the worker and the victim alone.  The victim
# (victim.c) is an ordinary timeshare process pinned to W, in one of the
# MODES:
#   timer  a 1 kHz timer doing WORK_US (default 100) of CPU per wake:
#          lateness and turnaround avg/p99/max in us (a keystroke)
#   spin   a CPU-bound loop: its CPU share of W (a compile job)
#   exec   fork+exec of /usr/bin/true every 20 ms: wall per exec (a shell)
#   rtt    the spinner again, and the flow's own TCP round-trip time from
#          iperf3's JSON report (TCP_INFO of the sender, us): what the
#          worker's absences cost the traffic it serves
#   unpinned  the timer unpinned under a whole-machine load of LOADCONNS
#          connections (default 2 x ncpu, at most 128): the sluggish-ssh
#          case of NOTES.md 2026-10-20
# The same victim runs once idle first, as the reference.
#
# For kwq every mode runs once per value in RATIOS (default "10 100 500
# 750 900 990 1000"), written to kern.kwq.net.cap_permille, the per-CPU
# share limit of PLAN P4c; without that knob only 1000 (no limit) runs.
# KWQ_KNOBS="oid=value ..." sets other kwq knobs for the whole run (e.g.
# kern.kwq.net.yield_prio=223); the original values are restored.
# PAIR=pairtq PAIR_MOD=if_pair_tq runs the taskqueue driver as the
# reference (lib.sh); it has no ratios.
#
# Per run one line: driver, ratio, mode, receiver Gbit/s, W, the kwq
# worker's busy share of W and its cap counters per second over the
# measurement window (passes, yields, cap demotions/yields/restores, cap
# ns share, passes run over the allowance with nothing else runnable),
# then the victim's own line.  A measurement harness: it
# fails only if a run does not complete.  Requires iperf3 and cc; skipped
# if missing.
. "$(dirname "$0")/lib.sh"

command -v iperf3 >/dev/null 2>&1 || { log "SKIP: iperf3 not installed"; exit 0; }
command -v cc >/dev/null 2>&1 || { log "SKIP: no cc for victim.c"; exit 0; }
test_init

SECS=${BENCH_SECS:-10}
CONNS=${CONNS:-1}
MODES=${MODES:-"timer spin exec rtt unpinned"}
RATIOS=${RATIOS:-"10 100 500 750 900 990 1000"}
WORK_US=${WORK_US:-100}
MTU=${PAIR_MTU:-16384}
NCPU=$(sysctl -n hw.ncpu)
_lc=$((NCPU * 2)); [ "$_lc" -gt 128 ] && _lc=128	# iperf3 -P maximum
LOADCONNS=${LOADCONNS:-$_lc}
OUT=${OUT_DIR:-$(mktemp -d /tmp/${TEST}.XXXXXX)}
[ -n "$OUT_DIR" ] || cleanup_push "rm -rf $OUT"
LEAD=3					# discovery seconds (1 s of counting inside)
WIN=$((SECS - LEAD - 2))		# the victim's window inside the run
[ "$WIN" -ge 3 ] || fail "BENCH_SECS must be at least 8"

must "build victim" cc -O2 -o "$OUT/victim" "$(dirname "$0")/victim.c"
VICTIM=$OUT/victim
has_oid() { sysctl -n "$1" >/dev/null 2>&1; }

if [ "$PAIR" = pair ]; then
	for kv in ${KWQ_KNOBS:-}; do
		_o=${kv%%=*}
		_orig=$(sysctl -n "$_o") || fail "no such knob $_o"
		cleanup_push "sysctl -q $_o=$_orig"
		must "set $kv" sysctl -q "$kv"
	done
	HAVE_CAP=no
	if has_oid kern.kwq.net.cap_permille; then
		HAVE_CAP=yes
		cleanup_push "sysctl -q kern.kwq.net.cap_permille=$(sysctl -n kern.kwq.net.cap_permille)"
		[ -n "${CAP_PERIOD:-}" ] && {
			cleanup_push "sysctl -q kern.kwq.net.cap_period_us=$(sysctl -n kern.kwq.net.cap_period_us)"
			must "cap_period_us=$CAP_PERIOD" sysctl -q kern.kwq.net.cap_period_us="$CAP_PERIOD"; }
	else
		log "note: this kwq has no cap_permille knob; ratio 1000 only"
		RATIOS=1000
	fi
	TPREFIX="kwq_net/"
else
	RATIOS=tq
	TPREFIX="${PAIR}_task_"
fi

create_pair
JS=$(jname s1); JC=$(jname c1)
mkjail "$JS"; mkjail "$JC"
must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
must "mtu" sh -c "jexec $JS ifconfig $PAIRA mtu $MTU && jexec $JC ifconfig $PAIRB mtu $MTU"
must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet 10.98.3.1/32 10.98.3.2 up && jexec $JC ifconfig $PAIRB inet 10.98.3.2/32 10.98.3.1 up"
must_retry 5 "ping" jexec "$JC" ping -q -o -c 2 -t 2 10.98.3.1

# CPU ids may have holes: take the list from this process's set.
CPUS=$(cpuset -g -p $$ | sed -n 's/.*mask: //p' | tr -d ',')
others_than() {	# all CPUs but $1, comma separated, for cpuset -l
	echo "$CPUS" | tr ' ' '\n' | grep -vx "$1" | paste -sd, -
}

rate_of() {
	if head -c 1 "$1" | grep -q '{'; then	# iperf3 -J
		awk '/"sum_received"/ { f = 1 } f && /bits_per_second/ { gsub(/[,"]/, ""); printf("%.2f\n", $2 / 1e9); exit }' "$1"
		return
	fi
	awk '/receiver/ { for (i = 1; i <= NF; i++) if ($i == "Gbits/sec") { r = $(i - 1); if ($0 ~ /^\[SUM\]/) s = r } }
	    END { print (s != "" ? s : (r != "" ? r : "FAILED")) }' "$1"
}
rtt_of() {	# sender TCP_INFO over the run: mean and max RTT, us
	awk '/"sender"/ { f = 1 } f && /"mean_rtt"/ { gsub(/[,"]/, ""); m = $2 } f && /"max_rtt"/ { gsub(/[,"]/, ""); x = $2 } f && /"min_rtt"/ { gsub(/[,"]/, ""); n = $2 }
	    END { printf("rtt_min=%s rtt_mean=%s rtt_max=%s", n, m, x) }' "$1"
}
busiest() {	# the worker CPU that did most in one second
	if [ "$PAIR" = pair ]; then
		a=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		sleep 1
		b=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		printf '%s\n%s\n' "$a" "$b" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print w }'
	else
		dtrace -qn "sched:::on-cpu /index(stringof(curthread->td_name), \"$TPREFIX\") == 0/ { @[cpu] = count(); } tick-1s { trunc(@, 1); printa(\"%d\\n\", @); exit(0); }" 2>/dev/null | awk 'NF { print $1; exit }'
	fi
}
# kwq worker counters on CPU $1: busy_ns passes yields cap_demotions cap_yields cap_restores cap_ns cap_runon
kwq_snap() {
	[ "$PAIR" = pair ] || { echo "0 0 0 0 0 0 0 0"; return; }
	_c=$(sysctl -n kern.kwq.net.cpu.$1.cap_demotions kern.kwq.net.cpu.$1.cap_yields kern.kwq.net.cpu.$1.cap_restores kern.kwq.net.cpu.$1.cap_ns kern.kwq.net.cpu.$1.cap_runon 2>/dev/null | tr '\n' ' ')
	echo "$(sysctl -n kern.kwq.net.cpu.$1.busy_ns kern.kwq.net.cpu.$1.passes kern.kwq.net.cpu.$1.yields | tr '\n' ' ') ${_c:-0 0 0 0 0}"
}
victim_cmd() {	# mode -> the victim's argument list for the window
	case "$1" in
	timer|unpinned)	echo "timer $WIN $WORK_US" ;;
	spin|rtt)	echo "spin $WIN" ;;
	exec)		echo "exec $((WIN * 40)) 20" ;;
	esac
}

log "idle reference (unpinned): $($VICTIM timer 3 $WORK_US)"
log "idle reference (unpinned): $($VICTIM spin 2)"
log "idle reference (unpinned): $($VICTIM exec 50 20)"

jexec "$JS" iperf3 -s >/dev/null 2>&1 &
SPID=$!
cleanup_push "kill $SPID"
sleep 1

for ratio in $RATIOS; do
	if [ "${HAVE_CAP:-no}" = yes ]; then
		must "cap_permille=$ratio" sysctl -q kern.kwq.net.cap_permille="$ratio"
	fi
	for mode in $MODES; do
		conns=$CONNS; [ "$mode" = unpinned ] && conns=$LOADCONNS
		fmt="-f g"; [ "$mode" = rtt ] && fmt="-J"
		# shellcheck disable=SC2086
		jexec "$JC" iperf3 $fmt -c 10.98.3.1 -P "$conns" -t "$SECS" > "$OUT/iperf.$ratio.$mode" 2>&1 &
		CPID=$!
		sleep $((LEAD - 1))
		W=$(busiest)
		[ -n "$W" ] || W=-
		if [ "$mode" != unpinned ] && [ "$W" != - ]; then
			# W keeps the worker and the victim; iperf3 goes elsewhere.
			_o=$(others_than "$W")
			for p in $(pgrep -f "iperf3 -s") $(pgrep -f "iperf3 -[fJ]"); do
				cpuset -l "$_o" -p "$p" 2>/dev/null
			done
			pin="cpuset -l $W"
		else
			pin=""
		fi
		s0=$(kwq_snap "$W"); t0=$(date +%s)
		# shellcheck disable=SC2046
		vline=$($pin $VICTIM $(victim_cmd "$mode") 2>&1)
		s1=$(kwq_snap "$W"); t1=$(date +%s)
		wait $CPID 2>/dev/null
		rate=$(rate_of "$OUT/iperf.$ratio.$mode")
		[ "$mode" = rtt ] && vline="$vline $(rtt_of "$OUT/iperf.$ratio.$mode")"
		dt=$((t1 - t0)); [ "$dt" -gt 0 ] || dt=1
		kstat=$(printf '%s\n%s\n' "$s0" "$s1" | awk -v dt="$dt" '
		    NR == 1 { for (i = 1; i <= NF; i++) a[i] = $i }
		    NR == 2 { printf("busy=%.3f passes/s=%d yields/s=%d cap_dem/s=%d cap_yld/s=%d cap_rst/s=%d cap_share=%.3f cap_runon/s=%d",
			($1 - a[1]) / (dt * 1e9), ($2 - a[2]) / dt, ($3 - a[3]) / dt,
			($4 - a[4]) / dt, ($5 - a[5]) / dt, ($6 - a[6]) / dt, ($7 - a[7]) / (dt * 1e9), ($8 - a[8]) / dt) }')
		log "RUN drv=$PAIR ratio=$ratio mode=$mode conns=$conns rate=$rate W=$W $kstat | $vline"
		sleep 1
	done
done

pass
