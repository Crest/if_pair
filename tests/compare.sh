#!/bin/sh
# compare.sh - one battery against several builds of the drivers, so that
# commits of if_pair + kwq and the taskqueue driver can be plotted against
# each other on the same boot.
#
#   compare.sh STAGEFILE
#
# STAGEFILE has one stage per line:
#
#   LABEL kwq KWQ_KO IFPAIR_KO [sysctl=value ...] [kenv:name=value ...]
#   LABEL tq  -      IFPAIR_TQ_KO
#
# a kwq stage loads KWQ_KO then IFPAIR_KO (both from the same commit), sets
# the sysctls, and restores them after; a kenv: entry is set in the kernel
# environment before the load, for loader tunables such as
# kern.kwq.net.priority, and unset after the unload; a tq stage loads the
# taskqueue driver.  For kwq stages the worker's own counters over the
# tput1 and udp_load windows are recorded too (switched, yields, tick
# yields, share-limit demotions per second, and the longest
# doorbell-to-pass latency), where the stage has them.  Stages run interleaved: all of them once per repetition (REPS,
# default 1), so a drift of the machine over the hour lands on every stage
# alike.  Every number goes to OUT_TSV (required) as
# "stage<TAB>rep<TAB>test<TAB>metric<TAB>value", one row per metric, and
# to the log as one line per run; compare_report.sh turns the TSV into a
# table.  Nothing may be loaded when this starts, and no dtrace(1)
# consumer may run: an enabled probe set vetoes the unload of a module,
# which would make the next stage measure the previous one.
#
# What is measured, per stage and repetition, through one pair between two
# vnet jails (MTU 16384, both iperf3 processes and the UDP client kept off
# the worker CPU W with cpuset):
#   tput1     one iperf3 flow for SECS (default 8): receiver Gbit/s; the
#             kernel's own CPU accounting for W over the run (kern.cp_times:
#             sys, user, idle shares), which every stage and driver has
#   tput16    sixteen flows: Gbit/s
#   udp_idle  request-reply over UDP, nothing else running: RTT p50 / p99 /
#             max us and unanswered requests (latency and jitter of the
#             delivery path itself)
#   udp_load  the same beside the one flow, on the flow's worker CPU: what
#             the stage's scheduling does to a latency-sensitive flow that
#             shares the CPU with a bulk flow
#   spin      a CPU-bound process pinned to W beside the flow: its CPU
#             share, and W's sys/user/idle shares (fairness to the rest of
#             the system); the flow's Gbit/s meanwhile (what fairness costs)
#   timer     a 1 kHz timeshare timer with 100 us of work per wake pinned to
#             W beside the flow: lateness p99 / max us
#   exec      fork+exec of /usr/bin/true every 20 ms pinned to W beside the
#             flow: avg / p99 us
#   machine   the timer unpinned under LOADCONNS connections (default
#             2 x ncpu, at most 128): lateness p99 / max us; the
#             whole-machine case, ULE's rather than the driver's
# The pair hashes a flow's addresses, protocol and ports to a worker CPU
# with a seed drawn at every module load, so the ports that put both flows
# on the CPU under test (WCPU, default 1) differ per stage: after each load
# the seed is read from the module (victim seed) and the iperf3 client ports
# and the UDP source port are computed offline (victim ports): the bulk
# flow's data and the UDP requests land on WCPU, their reverse directions
# where their own hash says.  If the seed cannot be read the ports
# are found by probing instead, which is slow on a machine with many CPUs.
# OUT_DIR keeps every iperf3 report (default: a temporary directory).
#
# TESTS selects the tests (default: the eight above).  The P4d tests
# (PLAN P4d: what running passes at idle priority does) are off by default
# and need dtrace for most of their numbers; COMPETITOR (timer, the
# default, or spin) is the thread pinned to W beside the flow in them: a
# timer sleeps and wakes a thousand times a second, which is what makes a
# worker that ran on at idle priority get preempted mid-pass; burst is the
# same timer with 400 us of work per wake; a spinner never lets the worker
# run at idle priority at all.
#   preempt   the worker's own absences while over its allowance: for every
#             switch-out that is not a sleep, voluntary (vol_*) or a
#             preemption (pre_*), the time until it runs again: count/s,
#             avg and max us, and how many lasted over 100 and 500 us
#   coloc     the flow's receiver pinned to W, the client off it: Gbit/s,
#             passes/s, items per pass, switched/s (the wake-and-be-
#             preempted pattern)
#   rwlock    a UDP echo server that disconnects its own socket every 2 ms
#             (INP_WLOCK) while the worker delivers to it (INP_RLOCK) with
#             the competitor on W: the disconnect latency, lockstat rw-block
#             counts and durations on udpinp, the UDP RTT
#   xcpu      one UDP echo socket fed by two request-reply flows, one
#             hashing to W and one to W2 = W + 1, both workers taking the
#             socket's inp read lock and receive-buffer mutex: W2's
#             passes/s and switched/s, both flows' RTT p99/max, lockstat
#             adaptive-block and rw-block counts and durations on W2 while
#             the competitor runs on W
#   twoq      a second pair whose flow also lands on W: per-queue items,
#             overruns, debts and parks of both pairs' queues on W, the
#             two flows' rates and the items share (accounting under
#             preemption)
#   churn     connection churn (connect/close loop) and address add/remove
#             on the pair during the flow with the competitor: connection
#             setup p99/max, alias operation max ms, epoch_wait_preempt
#             and waiting smr_poll counts and max us
#   magnet    an unpinned timer bystander during the flow with the
#             competitor on W: the share of its on-cpu events that landed
#             on W (sched_pickcpu() preferring the preemptible CPU)
#   fair      eight flows, spinner on W: per-stream receiver Gbit/s min,
#             max and coefficient of variation (fairness by CPU)
# machine additionally records interrupts/s from vmstat -i (the deadline
# callouts at scale).  tq stages skip the P4d tests except coloc, fair and
# churn's user-visible parts.
#
# Requires iperf3 and cc (victim.c is built on the fly).  Runs as root.
. "$(dirname "$0")/lib.sh"

command -v iperf3 >/dev/null 2>&1 || fail "iperf3 not installed"
command -v cc >/dev/null 2>&1 || fail "no cc for victim.c"
[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -n "$1" ] && [ -f "$1" ] || fail "usage: compare.sh STAGEFILE"
[ -n "${OUT_TSV:-}" ] || fail "set OUT_TSV to the result file"
STAGEFILE=$1

REPS=${REPS:-1}
TESTS=${TESTS:-"tput1 tput16 udp_idle udp_load spin timer exec machine"}
COMPETITOR=${COMPETITOR:-timer}
SECS=${SECS:-8}
UDPSECS=${UDPSECS:-4}
MTU=${PAIR_MTU:-16384}
NCPU=$(sysctl -n hw.ncpu)
_lc=$((NCPU * 2)); [ "$_lc" -gt 128 ] && _lc=128
LOADCONNS=${LOADCONNS:-$_lc}
if [ -n "${OUT_DIR:-}" ]; then
	OUT=$OUT_DIR; mkdir -p "$OUT"
else
	OUT=$(mktemp -d /tmp/compare.XXXXXX); cleanup_push "rm -rf $OUT"
fi
SRV=10.98.4.1; CLI=10.98.4.2; UDPPORT=47000; TCPPORT=5201
SRV2=10.98.6.1; CLI2=10.98.6.2; TCPPORT2=5202; UDPPORT2=47001; ECHOPORT=47100
command -v dtrace >/dev/null 2>&1 && HAVE_DTRACE=yes || HAVE_DTRACE=no
WCPU=${WCPU:-1}
CPUS=$(cpuset -g -p $$ | sed -n 's/.*mask: //p' | tr -d ',')

must "build victim" cc -O2 -o "$OUT/victim" "$(dirname "$0")/victim.c"
VICTIM=$OUT/victim

for m in kwq if_pair if_pair_tq; do
	kldstat -q -m $m && fail "$m is loaded; unload everything before comparing"
done
pgrep -q dtrace && fail "a dtrace consumer is running; it would veto the module unloads"

row() {	# test metric value
	printf '%s\t%s\t%s\t%s\t%s\n' "$STAGE" "$REP" "$1" "$2" "$3" >> "$OUT_TSV"
}
others_than() { echo "$CPUS" | tr ' ' '\n' | grep -vx "$1" | paste -sd, -; }
cp_times() {	# W -> "user nice sys intr idle" of that CPU
	sysctl -n kern.cp_times | awk -v w="$1" '{ b = 5 * w; print $(b + 1), $(b + 2), $(b + 3), $(b + 4), $(b + 5) }'
}
cp_shares() {	# "before" "after" -> "sys=x user=y idle=z" as shares of the interval
	printf '%s\n%s\n' "$1" "$2" | awk 'NR == 1 { for (i = 1; i <= 5; i++) a[i] = $i }
	    NR == 2 { t = 0; for (i = 1; i <= 5; i++) { d[i] = $i - a[i]; t += d[i] }
	        if (t <= 0) t = 1; printf("sys=%.3f user=%.3f idle=%.3f", (d[3] + d[4]) / t, (d[1] + d[2]) / t, d[5] / t) }'
}
rate_of() {
	awk '/receiver/ { for (i = 1; i <= NF; i++) if ($i == "Gbits/sec") { r = $(i - 1); if ($0 ~ /^\[SUM\]/) s = r } }
	    END { print (s != "" ? s : (r != "" ? r : 0)) }' "$1"
}
busiest_kwq() {	# the kwq worker CPU whose passes grew most over one second
	a=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }'); sleep 1
	b=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
	printf '%s\n%s\n' "$a" "$b" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print w }'
}
iperf_bg() {	# conns cport pin -> starts the client, sets IPID and IPERF (its report)
	_pin=""; [ -n "$3" ] && _pin="cpuset -l $3"
	_cp=""; [ -n "$2" ] && _cp="--cport $2"
	IPERF=$OUT/iperf.$STAGE.$REP.$TEST
	# shellcheck disable=SC2086
	$_pin jexec "$JC" iperf3 -f g -c $SRV -P "$1" $_cp -t "$SECS" > "$IPERF" 2>&1 &
	IPID=$!
}
iperf_rate() {	# after wait: the rate, and the error line if the run failed
	r=$(rate_of "$IPERF")
	[ "$r" = 0 ] && log "WARNING: $STAGE $TEST: iperf3 reported no rate: $(grep -v '^$' "$IPERF" | tail -1)"
	echo "$r"
}

stage_up() {	# KIND KWQ_KO IFPAIR_KO knobs...
	KIND=$1; KWQ_KO=$2; IFPAIR_KO=$3; shift 3
	KNOBS=""; KENVS=""
	for kv in "$@"; do
		case $kv in
		kenv:*) must "kenv ${kv#kenv:}" kenv "${kv#kenv:}"; KENVS="$KENVS ${kv#kenv:}" ;;
		esac
	done
	if [ "$KIND" = kwq ]; then
		must "load $KWQ_KO" kldload "$KWQ_KO"
		must "load $IFPAIR_KO" kldload "$IFPAIR_KO"
		PAIR=pair; MOD=if_pair
		for kv in "$@"; do
			case $kv in kenv:*) continue;; esac
			_o=${kv%%=*}; _orig=$(sysctl -n "$_o") || fail "stage $STAGE: no knob $_o"
			KNOBS="$KNOBS $_o=$_orig"
			must "set $kv" sysctl -q "$kv"
		done
	else
		must "load $IFPAIR_KO" kldload "$IFPAIR_KO"
		PAIR=pairtq; MOD=if_pair_tq
	fi
	PAIRA=$(ifconfig "$PAIR" create) || fail "ifconfig $PAIR create"
	PAIRB=${PAIRA%a}b
	JS=$(jname s); JC=$(jname c)
	mkjail "$JS"; mkjail "$JC"
	must "move $PAIRA" ifconfig "$PAIRA" vnet "$JS"
	must "move $PAIRB" ifconfig "$PAIRB" vnet "$JC"
	must "mtu" sh -c "jexec $JS ifconfig $PAIRA mtu $MTU && jexec $JC ifconfig $PAIRB mtu $MTU"
	must "addresses" sh -c "jexec $JS ifconfig $PAIRA inet $SRV/32 $CLI up && jexec $JC ifconfig $PAIRB inet $CLI/32 $SRV up"
	must_retry 5 "smoke ping" jexec "$JC" ping -q -o -c 2 -t 2 $SRV
	jexec "$JS" "$VICTIM" udpecho $UDPPORT >/dev/null 2>&1 &
	UPID=$!
	jexec "$JS" iperf3 -s >/dev/null 2>&1 &
	SPID=$!
	jexec "$JS" "$VICTIM" tcpecho $ECHOPORT >/dev/null 2>&1 &
	EPID=$!
	PAIR2A=""
	sleep 0.5
}
stage_down() {
	kill $UPID $SPID $EPID 2>/dev/null; wait $UPID $SPID $EPID 2>/dev/null
	if pgrep -q dtrace; then log "WARNING: a dtrace consumer is still running; waiting"; while pgrep -q dtrace; do sleep 1; done; fi
	[ -n "${SPID2:-}" ] && { kill $SPID2 2>/dev/null; wait $SPID2 2>/dev/null; SPID2=""; }
	[ -n "$PAIR2A" ] && ifconfig "$PAIR2A" destroy 2>/dev/null
	jail -f /dev/null -r "$JS" >/dev/null 2>&1; jail -f /dev/null -r "$JC" >/dev/null 2>&1
	ifconfig "$PAIRA" destroy 2>/dev/null
	for kv in $KNOBS; do sysctl -q "$kv" >/dev/null; done
	kldunload "$MOD" || fail "stage $STAGE: kldunload $MOD failed (pgrep dtrace?)"
	[ "$KIND" = kwq ] && { kldunload kwq || fail "stage $STAGE: kldunload kwq failed"; }
	for kv in $KENVS; do kenv -u "${kv%%=*}" >/dev/null 2>&1; done
	CLEANUP=$(printf '%s\n' "$CLEANUP" | grep -v "jail -f /dev/null -r ifp_\|ifconfig .* destroy\|kldunload")
}

ports() {	# the ports that put both flows on WCPU under this load's seed
	seed=$("$VICTIM" seed "$MOD.ko" 2>/dev/null)
	if [ -n "$seed" ]; then
		# sixteen TCP ports: one per run that uses the single flow, so no
		# run meets the previous run's TIME_WAIT entry ("unable to connect
		# stream: Address already in use", seen on a07 and lab)
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$WCPU" $CLI $SRV $TCPPORT $UDPPORT 16) || fail "no ports for CPU $WCPU"
		CPORTS=$(echo "${1#cports=}" | tr ',' ' '); UPORT=${2#uport=}
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$WCPU" $CLI $SRV $TCPPORT2 $UDPPORT2 2) || fail "no ports for CPU $WCPU (second set)"
		CPORTS2=$(echo "${1#cports=}" | tr ',' ' '); UPORT2=${2#uport=}
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$WCPU" $CLI2 $SRV2 $TCPPORT2 $UDPPORT 2) || fail "no ports for CPU $WCPU (second pair)"
		CPORTS3=$(echo "${1#cports=}" | tr ',' ' ')
		W2=$(( (WCPU + 1) % NCPU ))
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$W2" $CLI $SRV $TCPPORT $UDPPORT2 1) || fail "no ports for CPU $W2"
		UPORT2B=${2#uport=}
		SEED=$seed
		log "$STAGE: seed $seed, worker CPU $WCPU, iperf3 client ports $CPORTS, UDP source port $UPORT"
		return
	fi
	[ "$KIND" = kwq ] || fail "$STAGE: cannot read the hash seed of $MOD and cannot probe a taskqueue driver"
	discover
}
discover() {	# find a worker CPU and the ports that hash to it by probing (kwq only)
	log "discovering the worker CPU and the ports that hash to it"
	p=40001
	while [ $p -lt 40064 ]; do
		jexec "$JC" iperf3 -c $SRV -P 1 --cport $p -t 3 >/dev/null 2>&1 &
		_pid=$!; sleep 1
		w=$(busiest_kwq); wait $_pid
		[ -n "$w" ] && { CPORT=$p; WCPU=$w; break; }
		p=$((p + 1))
	done
	[ -n "${WCPU:-}" ] || fail "no worker CPU found for the iperf3 flow"
	u=41001
	while [ $u -lt 41400 ]; do
		a=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		jexec "$JC" "$VICTIM" udp $SRV $UDPPORT 1 $u 200 >/dev/null 2>&1
		b=$(sysctl kern.kwq.net.cpu | awk -F'[.:]' '$6 == "passes" { print $5, $NF }')
		w=$(printf '%s\n%s\n' "$a" "$b" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print w }')
		[ "$w" = "$WCPU" ] && { UPORT=$u; break; }
		u=$((u + 1))
	done
	[ -n "${UPORT:-}" ] || fail "no UDP source port hashes to CPU $WCPU"
	CPORTS=$CPORT
	log "worker CPU $WCPU, iperf3 client port $CPORT, UDP source port $UPORT"
}
cport_next() {	# the next unused TCP client port of this stage (wraps)
	set -- $CPORTS; CPORT=$1; shift; CPORTS="$* $CPORT"
}
verify_w() {	# after a flow on CPORT: did W see it (kwq counters, or W's sys share)?
	case $1 in
	kwq) [ "$(busiest_kwq_delta "$2" "$3")" = "$WCPU" ] || log "WARNING: $STAGE: the flow did not run on CPU $WCPU" ;;
	esac
}
busiest_kwq_delta() {	# two snapshots of the per-CPU pass counters -> the CPU that grew most
	printf '%s\n%s\n' "$1" "$2" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print w }'
}
kwq_passes() { sysctl kern.kwq.net.cpu 2>/dev/null | awk -F'[.:]' '$6 == "passes" { print $5, $NF }'; }
oid0() { sysctl -n "$1" 2>/dev/null || echo 0; }	# a counter an older stage may lack
kwq_counters() {	# W -> "switched yields tick_yields cap_demotions" (0 where absent)
	echo "$(oid0 kern.kwq.net.cpu.$1.switched) $(oid0 kern.kwq.net.cpu.$1.yields) $(oid0 kern.kwq.net.cpu.$1.tick_yields) $(oid0 kern.kwq.net.cpu.$1.cap_demotions)"
}
kwq_rows() {	# TEST before after secs: the worker's counters per second and the longest doorbell-to-pass latency
	[ "$KIND" = kwq ] || return 0
	printf '%s\n%s\n' "$2" "$3" | awk -v dt="$4" 'NR == 1 { for (i = 1; i <= 4; i++) a[i] = $i }
	    NR == 2 { printf("kwq_switched_s=%d kwq_yields_s=%d kwq_tick_yields_s=%d kwq_dem_s=%d", ($1 - a[1]) / dt, ($2 - a[2]) / dt, ($3 - a[3]) / dt, ($4 - a[4]) / dt) }' > "$OUT/kwqrows"
	_ml=$(( $(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.maxlat_ns") > $(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.maxlat_ns") ? $(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.maxlat_ns") : $(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.maxlat_ns") ))
	for kv in $(cat "$OUT/kwqrows") "kwq_maxlat_us=$((_ml / 1000))"; do row "$1" "${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP $1 $(cat "$OUT/kwqrows") kwq_maxlat_us=$((_ml / 1000))"
}

# A competitor pinned to W beside the flow for the P4d tests; sets KPID.
competitor_bg() {	# secs
	case $COMPETITOR in
	spin) cpuset -l "$WCPU" "$VICTIM" spin "$1" > "$OUT/competitor" 2>&1 & ;;
	burst) cpuset -l "$WCPU" "$VICTIM" timer "$1" 400 > "$OUT/competitor" 2>&1 & ;;
	*) cpuset -l "$WCPU" "$VICTIM" timer "$1" 100 > "$OUT/competitor" 2>&1 & ;;
	esac
	KPID=$!
}
# A dtrace consumer that exits by itself after SECS and writes to FILE; sets DPID.
dtrace_bg() {	# secs file script
	[ "$HAVE_DTRACE" = yes ] || { DPID=""; return; }
	dtrace -qn "$3 tick-${1}s { exit(0); }" -o "$2" > /dev/null 2>&1 &
	DPID=$!
}
dtrace_wait() { [ -n "$DPID" ] && wait $DPID 2>/dev/null; DPID=""; }
dval() {	# file key -> the value printed as "key value", else 0
	awk -v k="$2" '$1 == k { print $2; found = 1; exit } END { if (!found) print 0 }' "$1" 2>/dev/null
}
q_items() {	# pair-a pair-b cpu -> items overruns debts parks summed over both queues
	for f in items overruns debts parks; do
		echo $(( $(oid0 "kern.kwq.net.queue.$1.cpu.$3.$f") + $(oid0 "kern.kwq.net.queue.$2.cpu.$3.$f") ))
	done | tr '\n' ' '
}
vmstat_total() { vmstat -i | awk '/^Total/ { print $2 }'; }

t_tput1() {
	c0=$(cp_times "$WCPU"); k0=$(kwq_passes); q0=$(kwq_counters "$WCPU"); cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; wait $IPID; c1=$(cp_times "$WCPU"); k1=$(kwq_passes); q1=$(kwq_counters "$WCPU")
	r=$(iperf_rate); sh=$(cp_shares "$c0" "$c1")
	[ "$KIND" = kwq ] && verify_w kwq "$k0" "$k1"
	kwq_rows tput1 "$q0" "$q1" "$SECS"
	row tput1 gbit "$r"; for kv in $sh; do row tput1 "w_${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP tput1 gbit=$r W=$WCPU $sh"
}
t_tput16() {
	iperf_bg 16 "" ""; wait $IPID; r=$(iperf_rate)
	row tput16 gbit "$r"; log "$STAGE rep $REP tput16 gbit=$r"
}
t_udp_idle() {
	u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT $UDPSECS "$UPORT" 500)
	for kv in $u; do case $kv in rtt_*|lost=*) row udp_idle "${kv%%=*}" "${kv#*=}";; esac; done
	log "$STAGE rep $REP udp_idle $u"
}
t_udp_load() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	q0=$(kwq_counters "$WCPU")
	u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT $UDPSECS "$UPORT" 500)
	q1=$(kwq_counters "$WCPU"); wait $IPID; r=$(iperf_rate)
	kwq_rows udp_load "$q0" "$q1" "$UDPSECS"
	for kv in $u; do case $kv in rtt_*|lost=*) row udp_load "${kv%%=*}" "${kv#*=}";; esac; done
	row udp_load gbit "$r"; log "$STAGE rep $REP udp_load $u gbit=$r"
}
t_spin() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	c0=$(cp_times "$WCPU"); v=$(cpuset -l "$WCPU" "$VICTIM" spin $((SECS - 3))); c1=$(cp_times "$WCPU")
	wait $IPID; r=$(iperf_rate); sh=$(cp_shares "$c0" "$c1")
	sv=$(echo "$v" | sed -n 's/.*share=\([0-9.]*\).*/\1/p')
	row spin share "$sv"; row spin gbit "$r"; for kv in $sh; do row spin "w_${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP spin share=$sv gbit=$r $sh"
}
t_timer() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	v=$(cpuset -l "$WCPU" "$VICTIM" timer $((SECS - 3)) 100); wait $IPID; r=$(iperf_rate)
	for kv in $v; do case $kv in late_p99=*|late_max=*|turn_p99=*) row timer "${kv%%=*}" "${kv#*=}";; esac; done
	row timer gbit "$r"; log "$STAGE rep $REP timer $v gbit=$r"
}
t_exec() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	v=$(cpuset -l "$WCPU" "$VICTIM" exec $(((SECS - 3) * 40)) 20); wait $IPID
	for kv in $v; do case $kv in exec_avg=*|exec_p99=*) row exec "${kv%%=*}" "${kv#*=}";; esac; done
	log "$STAGE rep $REP exec $v"
}
t_machine() {
	i0=$(vmstat_total); iperf_bg "$LOADCONNS" "" ""; sleep 2
	v=$("$VICTIM" timer $((SECS - 3)) 100); wait $IPID; r=$(iperf_rate); i1=$(vmstat_total)
	for kv in $v; do case $kv in late_p99=*|late_max=*) row machine "${kv%%=*}" "${kv#*=}";; esac; done
	row machine gbit "$r"; row machine intr_s $(( (i1 - i0) / SECS ))
	log "$STAGE rep $REP machine $v gbit=$r intr_s=$(( (i1 - i0) / SECS ))"
}

# ---- P4d tests (PLAN P4d).  WSECS is the measured window inside the flow.
WSECS=$((SECS - 3))
t_preempt() {
	[ "$KIND" = kwq ] || return 0
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 1
	competitor_bg "$WSECS"
	dtrace_bg "$WSECS" "$OUT/preempt.$STAGE.$REP" "
	sched:::preempt /curthread->td_name == \"kwq_net/$WCPU\"/ { self->pre = 1; }
	sched:::off-cpu /curthread->td_name == \"kwq_net/$WCPU\" && curthread->td_inhibitors == 0/ { self->t = timestamp; self->p = self->pre; self->pre = 0; }
	sched:::off-cpu /curthread->td_name == \"kwq_net/$WCPU\" && curthread->td_inhibitors != 0/ { self->pre = 0; self->t = 0; }
	sched:::on-cpu /curthread->td_name == \"kwq_net/$WCPU\" && self->t/ { this->d = (timestamp - self->t) / 1000; @n[self->p] = count(); @a[self->p] = avg(this->d); @m[self->p] = max(this->d); @g1[self->p] = sum(this->d >= 100); @g5[self->p] = sum(this->d >= 500); self->t = 0; }
	END { printa(\"n%d %@d\n\", @n); printa(\"avg%d %@d\n\", @a); printa(\"max%d %@d\n\", @m); printa(\"g100_%d %@d\n\", @g1); printa(\"g500_%d %@d\n\", @g5); }"
	dtrace_wait; wait $KPID; wait $IPID; r=$(iperf_rate)
	f=$OUT/preempt.$STAGE.$REP
	for k in vol:0 pre:1; do
		row preempt "${k%%:*}_s" $(( $(dval "$f" "n${k#*:}") / WSECS )); row preempt "${k%%:*}_avg_us" "$(dval "$f" "avg${k#*:}")"
		row preempt "${k%%:*}_max_us" "$(dval "$f" "max${k#*:}")"; row preempt "${k%%:*}_gt100" "$(dval "$f" "g100_${k#*:}")"; row preempt "${k%%:*}_gt500" "$(dval "$f" "g500_${k#*:}")"
	done
	row preempt gbit "$r"
	log "$STAGE rep $REP preempt vol/s=$(( $(dval "$f" n0) / WSECS )) vol_max=$(dval "$f" max0) pre/s=$(( $(dval "$f" n1) / WSECS )) pre_avg=$(dval "$f" avg1) pre_max=$(dval "$f" max1) pre_gt100=$(dval "$f" g100_1) pre_gt500=$(dval "$f" g500_1) gbit=$r competitor=$(cat "$OUT/competitor" | cut -c1-80)"
}
t_coloc() {
	# the receiver on W: a second iperf3 server pinned there, the flow's data to it
	cpuset -l "$WCPU" jexec "$JS" iperf3 -s -p $TCPPORT2 >/dev/null 2>&1 &
	_s2=$!; sleep 0.3
	set -- $CPORTS2; _cp=$1; CPORTS2="$2 $1"
	k0=$(kwq_passes); q0=$(kwq_counters "$WCPU"); i0=$(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.items"); j0=$(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.items")
	IPERF=$OUT/iperf.$STAGE.$REP.coloc
	cpuset -l "$OTHERS" jexec "$JC" iperf3 -f g -c $SRV -p $TCPPORT2 -P 1 --cport "$_cp" -t "$SECS" > "$IPERF" 2>&1
	k1=$(kwq_passes); q1=$(kwq_counters "$WCPU"); i1=$(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.items"); j1=$(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.items")
	kill $_s2 2>/dev/null; wait $_s2 2>/dev/null
	r=$(iperf_rate)
	pw=$(printf '%s\n%s\n' "$k0" "$k1" | awk -v w="$WCPU" '$1 == w { d = $2 - d } END { print d + 0 }')
	it=$(( i1 - i0 + j1 - j0 ))
	row coloc gbit "$r"; row coloc passes_s $(( pw / SECS )); row coloc items_per_pass "$(awk -v i="$it" -v p="$pw" 'BEGIN { printf("%.2f", p > 0 ? i / p : 0) }')"
	kwq_rows coloc "$q0" "$q1" "$SECS"
	log "$STAGE rep $REP coloc gbit=$r passes/s=$(( pw / SECS )) items/pass=$(awk -v i="$it" -v p="$pw" 'BEGIN { printf("%.2f", p > 0 ? i / p : 0) }')"
}
t_rwlock() {
	[ "$KIND" = kwq ] || return 0
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 1
	competitor_bg "$WSECS"
	cpuset -l "$OTHERS" jexec "$JS" "$VICTIM" udpecho2 $UDPPORT2 $((WSECS + 2)) 2 > "$OUT/udpecho2" 2>&1 &
	_e2=$!; sleep 0.3
	dtrace_bg "$WSECS" "$OUT/rwlock.$STAGE.$REP" "
	lockstat:::rw-block /stringof(((struct lock_object *)arg0)->lo_name) == \"udpinp\"/ { @n = count(); @t = sum(arg1); @m = max(arg1); }
	END { printa(\"n %@d\n\", @n); printa(\"t %@d\n\", @t); printa(\"m %@d\n\", @m); }"
	u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT2 $WSECS "$UPORT2" 500)
	dtrace_wait; wait $KPID; wait $_e2; wait $IPID; r=$(iperf_rate)
	w=$(cat "$OUT/udpecho2"); f=$OUT/rwlock.$STAGE.$REP
	for kv in $w; do case $kv in wl_*) row rwlock "${kv%%=*}" "${kv#*=}";; esac; done
	for kv in $u; do case $kv in rtt_p99=*|rtt_max=*|lost=*) row rwlock "udp_${kv%%=*}" "${kv#*=}";; esac; done
	row rwlock rwblock_s $(( $(dval "$f" n) / WSECS )); row rwlock rwblock_avg_us "$(awk -v n="$(dval "$f" n)" -v t="$(dval "$f" t)" 'BEGIN { printf("%.1f", n > 0 ? t / n / 1000 : 0) }')"; row rwlock rwblock_max_us $(( $(dval "$f" m) / 1000 ))
	log "$STAGE rep $REP rwlock $w $u rwblock/s=$(( $(dval "$f" n) / WSECS )) rwblock_max_us=$(( $(dval "$f" m) / 1000 )) gbit=$r"
}
t_xcpu() {
	# One UDP echo socket, two request-reply clients: one whose requests
	# hash to W (the competitor's CPU), one whose requests hash to W2.
	# Both workers deliver into the same socket, so they share its inp
	# read lock and its receive-buffer mutex across CPUs; W2's worker is
	# the uncontended one that pays for W's preemptions.  (A TCP flow
	# cannot be used: the stack learns the data direction's flow id and
	# sends its ACKs through the same CPU.)
	[ "$KIND" = kwq ] || return 0
	competitor_bg "$WSECS"
	cpuset -l "$OTHERS" jexec "$JS" "$VICTIM" udpecho2 $UDPPORT2 $((WSECS + 2)) 100000 > /dev/null 2>&1 &
	_e2=$!; sleep 0.3
	q0=$(kwq_counters "$W2"); k0=$(kwq_passes)
	dtrace_bg "$WSECS" "$OUT/xcpu.$STAGE.$REP" "
	lockstat:::adaptive-block /cpu == $W2/ { @n = count(); @t = sum(arg1); @m = max(arg1); @by[stringof(((struct lock_object *)arg0)->lo_name)] = count(); }
	lockstat:::rw-block /cpu == $W2/ { @rn = count(); @rt = sum(arg1); @rm = max(arg1); }
	END { printa(\"n %@d\\n\", @n); printa(\"t %@d\\n\", @t); printa(\"m %@d\\n\", @m); printa(\"rn %@d\\n\", @rn); printa(\"rt %@d\\n\", @rt); printa(\"rm %@d\\n\", @rm); printa(\"by_%s %@d\\n\", @by); }"
	cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT2 $WSECS "$UPORT2" 500 > "$OUT/udp_w" 2>&1 &
	_uw=$!
	u2=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT2 $WSECS "$UPORT2B" 500)
	wait $_uw; dtrace_wait; wait $KPID; wait $_e2
	q1=$(kwq_counters "$W2"); k1=$(kwq_passes)
	f=$OUT/xcpu.$STAGE.$REP; uw=$(cat "$OUT/udp_w")
	p2=$(printf '%s\n%s\n' "$k0" "$k1" | awk -v w="$W2" '$1 == w { d = $2 - d } END { print d + 0 }')
	set -- $q0; s0=$1; set -- $q1; s1=$1
	row xcpu w2 "$W2"; row xcpu w2_passes_s $(( p2 / WSECS )); row xcpu w2_switched_s $(( (s1 - s0) / WSECS ))
	for kv in $uw; do case $kv in rtt_p99=*|rtt_max=*) row xcpu "w_udp_${kv%%=*}" "${kv#*=}";; esac; done
	for kv in $u2; do case $kv in rtt_p99=*|rtt_max=*) row xcpu "w2_udp_${kv%%=*}" "${kv#*=}";; esac; done
	row xcpu w2_block_s $(( $(dval "$f" n) / WSECS )); row xcpu w2_block_avg_us "$(awk -v n="$(dval "$f" n)" -v t="$(dval "$f" t)" 'BEGIN { printf("%.1f", n > 0 ? t / n / 1000 : 0) }')"; row xcpu w2_block_max_us $(( $(dval "$f" m) / 1000 ))
	row xcpu w2_rwblock_s $(( $(dval "$f" rn) / WSECS )); row xcpu w2_rwblock_max_us $(( $(dval "$f" rm) / 1000 ))
	log "$STAGE rep $REP xcpu W2=$W2 w2_passes/s=$(( p2 / WSECS )) w2_switched/s=$(( (s1 - s0) / WSECS )) w_udp: $uw w2_udp: $u2 w2_block/s=$(( $(dval "$f" n) / WSECS )) w2_block_max_us=$(( $(dval "$f" m) / 1000 )) w2_rwblock/s=$(( $(dval "$f" rn) / WSECS )) w2_rwblock_max_us=$(( $(dval "$f" rm) / 1000 )) blocked_on: $(grep '^by_' "$f" 2>/dev/null | tr '\n' ' ')"
}
t_twoq() {
	[ "$KIND" = kwq ] || return 0
	if [ -z "$PAIR2A" ]; then
		PAIR2A=$(ifconfig "$PAIR" create) || fail "second pair"; PAIR2B=${PAIR2A%a}b
		ifconfig "$PAIR2A" vnet "$JS"; ifconfig "$PAIR2B" vnet "$JC"
		jexec "$JS" ifconfig "$PAIR2A" mtu $MTU; jexec "$JC" ifconfig "$PAIR2B" mtu $MTU
		jexec "$JS" ifconfig "$PAIR2A" inet $SRV2/32 $CLI2 up; jexec "$JC" ifconfig "$PAIR2B" inet $CLI2/32 $SRV2 up
		jexec "$JS" iperf3 -s -p $TCPPORT2 -B $SRV2 >/dev/null 2>&1 &
		SPID2=$!; sleep 0.5
	fi
	set -- $CPORTS3; _cp3=$1; CPORTS3="$2 $1"
	cport_next
	a0=$(q_items "$PAIRA" "$PAIRB" "$WCPU"); b0=$(q_items "$PAIR2A" "$PAIR2B" "$WCPU")
	iperf_bg 1 "$CPORT" "$OTHERS"
	IPERF2=$OUT/iperf2.$STAGE.$REP.twoq
	cpuset -l "$OTHERS" jexec "$JC" iperf3 -f g -c $SRV2 -p $TCPPORT2 -B $CLI2 -P 1 --cport "$_cp3" -t "$SECS" > "$IPERF2" 2>&1 &
	_i2=$!; sleep 1
	competitor_bg "$WSECS"; wait $KPID; wait $IPID; wait $_i2
	a1=$(q_items "$PAIRA" "$PAIRB" "$WCPU"); b1=$(q_items "$PAIR2A" "$PAIR2B" "$WCPU")
	r1=$(iperf_rate); r2=$(rate_of "$IPERF2")
	d=$(printf '%s\n%s\n%s\n%s\n' "$a0" "$a1" "$b0" "$b1" | awk 'NR == 1 { for (i = 1; i <= 4; i++) a[i] = $i } NR == 2 { for (i = 1; i <= 4; i++) da[i] = $i - a[i] } NR == 3 { for (i = 1; i <= 4; i++) b[i] = $i } NR == 4 { for (i = 1; i <= 4; i++) db[i] = $i - b[i]; printf("q1_items=%d q1_overruns=%d q1_debts=%d q1_parks=%d q2_items=%d q2_overruns=%d q2_debts=%d q2_parks=%d share1=%.3f", da[1], da[2], da[3], da[4], db[1], db[2], db[3], db[4], (da[1] + db[1]) > 0 ? da[1] / (da[1] + db[1]) : 0) }')
	for kv in $d; do row twoq "${kv%%=*}" "${kv#*=}"; done
	row twoq gbit1 "$r1"; row twoq gbit2 "$r2"
	log "$STAGE rep $REP twoq $d gbit1=$r1 gbit2=$r2"
}
t_churn() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 1
	competitor_bg "$WSECS"
	[ "$KIND" = kwq ] && dtrace_bg "$WSECS" "$OUT/churn.$STAGE.$REP" "
	fbt::epoch_wait_preempt:entry { self->e = timestamp; } fbt::epoch_wait_preempt:return /self->e/ { this->d = (timestamp - self->e) / 1000; @en = count(); @em = max(this->d); @es = sum(this->d); self->e = 0; }
	fbt::smr_poll:entry /arg2 != 0/ { self->s = timestamp; } fbt::smr_poll:return /self->s/ { this->d = (timestamp - self->s) / 1000; @sn = count(); @sm = max(this->d); @ss = sum(this->d); self->s = 0; }
	END { printa(\"en %@d\n\", @en); printa(\"em %@d\n\", @em); printa(\"es %@d\n\", @es); printa(\"sn %@d\n\", @sn); printa(\"sm %@d\n\", @sm); printa(\"ss %@d\n\", @ss); }"
	cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" tcpconn $SRV $ECHOPORT $((WSECS - 1)) 2 > "$OUT/tcpconn" 2>&1 &
	_c=$!
	# address churn on the server side of the pair, timed
	amax=0; n=0
	while [ $n -lt 20 ]; do
		t0=$(date +%s%N 2>/dev/null || echo 0)
		jexec "$JS" ifconfig "$PAIRA" inet 10.98.7.$((n + 1))/32 alias >/dev/null 2>&1
		jexec "$JS" ifconfig "$PAIRA" inet 10.98.7.$((n + 1)) -alias >/dev/null 2>&1
		t1=$(date +%s%N 2>/dev/null || echo 0)
		dt=$(( (t1 - t0) / 1000000 )); [ "$dt" -gt "$amax" ] && amax=$dt
		n=$((n + 1)); sleep 0.1
	done
	wait $_c; dtrace_wait; wait $KPID; wait $IPID; r=$(iperf_rate)
	c=$(cat "$OUT/tcpconn"); f=$OUT/churn.$STAGE.$REP
	for kv in $c; do case $kv in conn_p99=*|conn_max=*|failed=*) row churn "${kv%%=*}" "${kv#*=}";; esac; done
	row churn alias_max_ms "$amax"
	if [ "$KIND" = kwq ] && [ -f "$f" ]; then
		row churn epochwait_n "$(dval "$f" en)"; row churn epochwait_max_us "$(dval "$f" em)"; row churn smr_n "$(dval "$f" sn)"; row churn smr_max_us "$(dval "$f" sm)"
	fi
	row churn gbit "$r"
	log "$STAGE rep $REP churn $c alias_max_ms=$amax epochwait_n=$(dval "$f" en) epochwait_max_us=$(dval "$f" em) smr_n=$(dval "$f" sn) smr_max_us=$(dval "$f" sm) gbit=$r"
}
t_magnet() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 1
	competitor_bg "$WSECS"
	"$VICTIM" timer "$WSECS" 100 > "$OUT/bystander" 2>&1 &
	_b=$!
	dtrace_bg "$WSECS" "$OUT/magnet.$STAGE.$REP" "
	sched:::on-cpu /pid == $_b/ { @c[cpu] = count(); @all = count(); }
	END { printa(\"cpu%d %@d\n\", @c); printa(\"all %@d\n\", @all); }"
	wait $_b; dtrace_wait; wait $KPID; wait $IPID; r=$(iperf_rate)
	f=$OUT/magnet.$STAGE.$REP; v=$(cat "$OUT/bystander")
	sh=$(awk -v w="$WCPU" -v a="$(dval "$f" all)" -v o="$(dval "$f" "cpu$WCPU")" 'BEGIN { printf("%.3f", a > 0 ? o / a : 0) }')
	row magnet on_w_share "$sh"
	for kv in $v; do case $kv in late_p99=*|late_max=*) row magnet "by_${kv%%=*}" "${kv#*=}";; esac; done
	row magnet gbit "$r"
	log "$STAGE rep $REP magnet on_w_share=$sh bystander: $v gbit=$r"
}
t_fair() {
	cpuset -l "$WCPU" "$VICTIM" spin $((SECS + 2)) > "$OUT/competitor" 2>&1 &
	KPID=$!; sleep 0.3
	IPERF=$OUT/iperf.$STAGE.$REP.fair
	jexec "$JC" iperf3 -J -c $SRV -P 8 -t "$SECS" > "$IPERF" 2>&1
	wait $KPID
	st=$(awk '/"end"/ { e = 1 } e && /"receiver"/ { f = 1 } e && f && /bits_per_second/ { gsub(/[,"]/, ""); v[n++] = $2 / 1e9; f = 0 }
	    END { if (n == 0) { print "fair_min=0 fair_max=0 fair_cv=0 fair_sum=0"; exit } mn = v[0]; mx = v[0]; s = 0; for (i = 0; i < n; i++) { s += v[i]; if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i] } m = s / n; ss = 0; for (i = 0; i < n; i++) ss += (v[i] - m) * (v[i] - m); printf("fair_min=%.2f fair_max=%.2f fair_cv=%.1f fair_sum=%.2f", mn, mx, m > 0 ? sqrt(ss / n) / m * 100 : 0, s) }' "$IPERF")
	for kv in $st; do row fair "${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP fair $st"
}

battery() {
	OTHERS=$(others_than "$WCPU")
	cpuset -l "$OTHERS" -p "$SPID"		# the iperf3 server, off W
	cpuset -l "$OTHERS" -p "$EPID"
	for t in $TESTS; do
		TEST=$t	# also the iperf3 report's name; lib.sh's TEST only prefixed the log
		case $t in
		tput1|tput16|udp_idle|udp_load|spin|timer|exec|machine|preempt|coloc|rwlock|xcpu|twoq|churn|magnet|fair) t_$t ;;
		*) fail "unknown test $t" ;;
		esac
	done
}

log "stages: $(awk '{ printf("%s ", $1) }' "$STAGEFILE"); reps $REPS; results in $OUT_TSV"
REP=1
while [ "$REP" -le "$REPS" ]; do
	while read -r STAGE KIND KWQ_KO IFPAIR_KO KNOBLINE; do
		case $STAGE in ''|\#*) continue;; esac
		# shellcheck disable=SC2086
		stage_up "$KIND" "$KWQ_KO" "$IFPAIR_KO" $KNOBLINE
		ports
		battery
		stage_down
	done < "$STAGEFILE"
	REP=$((REP + 1))
done
pass
