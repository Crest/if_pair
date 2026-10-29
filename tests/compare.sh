#!/bin/sh
# compare.sh - one battery against several builds of the drivers, so that
# commits of if_pair + kwq and the taskqueue driver can be plotted against
# each other on the same boot.
#
#   compare.sh STAGEFILE
#
# STAGEFILE has one stage per line:
#
#   LABEL kwq KWQ_KO IFPAIR_KO [sysctl=value ...] [kenv:name=value ...] [jail:sysctl=value ...]
#   LABEL tq  -      IFPAIR_TQ_KO [jail:sysctl=value ...]
#
# a kwq stage loads KWQ_KO then IFPAIR_KO (both from the same commit), sets
# the sysctls, and restores them after; a kenv: entry is set in the kernel
# environment before the load, for loader tunables such as
# kern.kwq.net.priority, and unset after the unload; a jail: entry is set
# inside both vnet jails after they are created and before any socket exists
# in them (per-vnet knobs such as net.inet.tcp.functions_default, whose host
# value a new vnet does not inherit); a tq stage loads the taskqueue driver
# and accepts jail: entries too.  For kwq stages the worker's own counters over the
# tput1 and udp_load windows are recorded too (switched, yields, tick
# yields, share-limit demotions, hand-overs and budget cuts, slice
# yields per second, and the longest doorbell-to-pass latency), where the
# stage has them.  Stages run interleaved: all of them once per repetition (REPS,
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
# callouts at scale) and, since 2026-10-27, intr_w_timer_s and intr_timer_s:
# the timer interrupts of W and of all CPUs (vmstat -i's cpuN:timer lines on
# amd64; a dtrace count of fbt::handleevents:entry by CPU on arm64, where the
# generic timer is one line for all CPUs).  tq stages skip the P4d tests
# except coloc, fair and churn's user-visible parts.
# COMPETITOR=intr (kwq stages only) loads the battery's test module
# (KWQ_TEST_KO, the one kwq_test.ko of the battery) and runs its intr_load
# stressor on W: a direct callout every 100 us doing 20 us of work in
# interrupt context, the one preemptor the priority invariant permits
# (kwq/tests/SPEC-p4e.md 2.2); tq stages fall back to timer with a warning.
# SMT_SIBLINGS=exclude keeps the pinned load generators (the iperf3 client
# and server, the UDP and echo victims) off every hardware thread of W's
# core, read from kern.sched.topology_spec, instead of off W alone (the
# default, "share", which earlier batteries ran with).  On an SMT machine a
# generator that lands on W's sibling shares a core with the worker for the
# whole run; lab's single flow was bimodal across every stage and driver
# (WAKE.md S12.2), and this is the knob that tests that cause.  On a machine
# without SMT both settings give the same mask.
#
# Tests added for SPEC-p4e.md (off by default):
#   sparse    SPARSE_N request-reply UDP flows at SPARSE_RATE requests/s
#             each (default N "16 64" capped at 4 x ncpu, rates "1000 5000"),
#             source ports spread over the worker CPUs (victim uports), one
#             echo server: per (N, r) the flows' RTT p50 and p99 as min /
#             median / max over flows, lost, the sums over all NET workers
#             of wakeups, ticks and passes per second, items per pass, the
#             machine's interrupts and timer interrupts per second and its
#             system CPU share
#   udp_burst UDP request-reply in bursts (victim udpburst) for each B:G_us
#             in UDP_BURSTS (default "1:2000 16:200 64:2000 64:50000"):
#             first-in-burst and last-in-burst RTT p50 / p99, lost, and the
#             worker's wakeups, ticks and empty ticks per burst, with nothing
#             else running
#   udp_burst_load  the same beside the bulk flow on W
#   coloc     also reports both pair queues' overruns, overruns_switched,
#             parks and debts on W and the receiver's involuntary context
#             switches (procstat -r)
#   twoq      also reports overruns_switched per queue
# The kwq_* rows gained ticks, empty and single-element ticks, doorbells
# skipped by the tick, overruns and overruns_switched of both pair queues,
# prio_violations and the low-priority wakes alone (0 on builds without the
# counters).  They lost kwq_wake_count_s: the specification dropped the
# count thresholds, so there is no count-ended wake episode to report.
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
SPARSE_N=${SPARSE_N:-"16 64"}
SPARSE_RATE=${SPARSE_RATE:-"1000 5000"}
UDP_BURSTS=${UDP_BURSTS:-"1:2000 16:200 64:2000 64:50000"}
KWQ_TEST_KO=${KWQ_TEST_KO:-}
ARCH=$(uname -p)
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
smt_siblings() {	# W -> the CPUs on W's core, W included (comma-separated); W alone without SMT
	sysctl -n kern.sched.topology_spec 2>/dev/null | awk -v w="$1" '
	    /<cpu count=/ { s = $0; sub(/.*">/, "", s); sub(/<\/cpu>.*/, "", s); gsub(/ /, "", s); last = s; next }
	    /SMT group/ { n = split(last, c, ","); for (i = 1; i <= n; i++) if (c[i] == w) { print last; found = 1; exit } }
	    END { if (!found) print w }'
}
others_than_core() {	# W -> every CPU but the hardware threads of W's core
	_ex=$(smt_siblings "$1" | tr ',' '\n')
	echo "$CPUS" | tr ' ' '\n' | grep -vxF "$_ex" | paste -sd, -
}
cp_times() {	# W -> "user nice sys intr idle" of that CPU
	sysctl -n kern.cp_times | awk -v w="$1" '{ b = 5 * w; print $(b + 1), $(b + 2), $(b + 3), $(b + 4), $(b + 5) }'
}
cp_shares() {	# "before" "after" -> "sys=x user=y idle=z" as shares of the interval
	printf '%s\n%s\n' "$1" "$2" | awk 'NR == 1 { for (i = 1; i <= 5; i++) a[i] = $i }
	    NR == 2 { t = 0; for (i = 1; i <= 5; i++) { d[i] = $i - a[i]; t += d[i] }
	        if (t <= 0) t = 1; printf("sys=%.3f user=%.3f idle=%.3f intr=%.3f", (d[3] + d[4]) / t, (d[1] + d[2]) / t, d[5] / t, d[4] / t) }'
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
gen_sample() {	# client-pid server-pid out: three times in the run, did a thread of the
		# iperf3 client / server last run on a hardware thread of W's core?
		# Writes "cli srv" as fractions of the samples (WAKE.md S12.2).
	_core=",$(smt_siblings "$WCPU"),"; _cs=0; _ss=0
	for _i in 1 2 3; do
		sleep $(( SECS / 4 ))
		_c=$(ps -H -o cpu= -p "$1" 2>/dev/null | awk -v c="$_core" '{ if (index(c, "," $1 ",")) h = 1 } END { print h + 0 }')
		_s=$(ps -H -o cpu= -p "$2" 2>/dev/null | awk -v c="$_core" '{ if (index(c, "," $1 ",")) h = 1 } END { print h + 0 }')
		_cs=$((_cs + _c)); _ss=$((_ss + _s))
	done
	awk -v c="$_cs" -v s="$_ss" 'BEGIN { printf("%.2f %.2f\n", c / 3, s / 3) }' > "$3"
}
iperf_rate() {	# after wait: the rate, and the error line if the run failed
	r=$(rate_of "$IPERF")
	[ "$r" = 0 ] && log "WARNING: $STAGE $TEST: iperf3 reported no rate: $(grep -v '^$' "$IPERF" | tail -1)"
	echo "$r"
}

stage_up() {	# KIND KWQ_KO IFPAIR_KO knobs...
	KIND=$1; KWQ_KO=$2; IFPAIR_KO=$3; shift 3
	KNOBS=""; KENVS=""; JKNOBS=""
	for kv in "$@"; do
		case $kv in
		kenv:*) must "kenv ${kv#kenv:}" kenv "${kv#kenv:}"; KENVS="$KENVS ${kv#kenv:}" ;;
		jail:*) JKNOBS="$JKNOBS ${kv#jail:}" ;;
		esac
	done
	if [ "$KIND" = kwq ]; then
		must "load $KWQ_KO" kldload "$KWQ_KO"
		must "load $IFPAIR_KO" kldload "$IFPAIR_KO"
		if [ "$COMPETITOR" = intr ]; then
			[ -n "$KWQ_TEST_KO" ] || fail "COMPETITOR=intr needs KWQ_TEST_KO"
			must "load $KWQ_TEST_KO" kldload "$KWQ_TEST_KO"
		fi
		PAIR=pair; MOD=if_pair
		for kv in "$@"; do
			case $kv in kenv:*|jail:*) continue;; esac
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
	for kv in $JKNOBS; do must "jail knob $kv" sh -c "jexec $JS sysctl -q $kv && jexec $JC sysctl -q $kv"; done
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
	# A vnet jail dies asynchronously: jail -r returns, the jail stays
	# listed by jls -d while anything still references it (its exiting
	# processes, TIME_WAIT sockets for their 2MSL), and the moment the
	# last reference drops it leaves the list and prison_deref destroys
	# its vnet.  Unloading the cloner during that vnet_destroy deadlocks
	# the kernel (vnet_destroy holds ifnet_detach_sxlock and wants the
	# vnet sysinit lock, vnet_deregister_sysuninit holds that and wants
	# the former: the guest, 2026-10-27, reproduced with stock epair).
	# So wait until both jails have left the list, then a further two
	# seconds for the vnet teardown that follows their disappearance,
	# which userland cannot observe.  Mitigation, not a fix.
	n=0; while jls -d name 2>/dev/null | grep -qx -e "$JS" -e "$JC"; do
		[ $n -ge 200 ] && { log "WARNING: $JS/$JC still dying after 20 s; unloading anyway"; break; }
		sleep 0.1; n=$((n+1))
	done
	sleep 2
	ifconfig "$PAIRA" destroy 2>/dev/null
	for kv in $KNOBS; do sysctl -q "$kv" >/dev/null; done
	if kldstat -q -m kwq_test; then intr_stress_stop; sleep 0.3; kldunload kwq_test || fail "stage $STAGE: kldunload kwq_test failed"; fi
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
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$WCPU" $CLI $SRV $TCPPORT2 $UDPPORT2 16) || fail "no ports for CPU $WCPU (second set)"
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
q2() {	# field cpu -> the field summed over both pair queues on that CPU (0 where absent)
	echo $(( $(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$2.$1") + $(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$2.$1") ))
}
# kwq_counters prints one 26-field vector; the fields are positional, so a new
# one is always appended at the end and no index ever moves (kwq_rows,
# udp_burst_run and any later reader index into it).  In order:
#
#    1 switched           2 yields              3 tick_yields       4 cap_demotions
#    5 wakeups            6 passes              7 items (both pair queues on W)
#    8 wakes_deferred     9 wake_defer_ns      10 wakes_low        11 wake_low_ns
#   12 reserved (the count-threshold wakes of the kwq-wake-batch branch; the
#      specification dropped count thresholds, so no row is emitted from it any
#      more, but the slot stays so the indices below it do not move)
#   13 wakes_deadline + wake_promotions_deadline
#   14 db_batched        15 db_flushed         16 wake_idle_cpu
#   17 cap_yields        18 cap_cuts           19 slice_yields
#   20 ticks             21 ticks_empty        22 ticks_single
#   23 doorbells_skipped (both queues)
#   24 overruns (both queues)                  25 overruns_switched (both queues)
#   26 prio_violations
#
# Fields 8, 9, 12, 14, 15 and 16 exist only on the wake-moderation campaign's
# branches and read 0 here; 20..26 are the tick regime's (SPEC-p4e.md S1) and
# read 0 on a build without them.
kwq_counters() {	# W -> the 26 fields above (0 where absent)
	_w=kern.kwq.net.cpu.$1
	echo "$(oid0 $_w.switched) $(oid0 $_w.yields) $(oid0 $_w.tick_yields) $(oid0 $_w.cap_demotions) $(oid0 $_w.wakeups) $(oid0 $_w.passes) $(q2 items "$1") $(oid0 $_w.wakes_deferred) $(oid0 $_w.wake_defer_ns) $(oid0 $_w.wakes_low) $(oid0 $_w.wake_low_ns) $(( $(oid0 $_w.wakes_count) + $(oid0 $_w.wake_promotions_count) )) $(( $(oid0 $_w.wakes_deadline) + $(oid0 $_w.wake_promotions_deadline) )) $(oid0 $_w.db_batched) $(oid0 $_w.db_flushed) $(oid0 $_w.wake_idle_cpu) $(oid0 $_w.cap_yields) $(oid0 $_w.cap_cuts) $(oid0 $_w.slice_yields) $(oid0 $_w.ticks) $(oid0 $_w.ticks_empty) $(oid0 $_w.ticks_single) $(q2 doorbells_skipped "$1") $(q2 overruns "$1") $(q2 overruns_switched "$1") $(oid0 $_w.prio_violations)"
}
kwq_rows() {	# TEST before after secs: the worker's counters per second and the longest doorbell-to-pass latency
	[ "$KIND" = kwq ] || return 0
	# The wake-moderation rows (PLAN P4e): wakeups/s, items per pass, deferred or
	# low-priority wake episodes per second and their mean length in us, the
	# low-priority wakes alone and the episodes a deadline ended; and the tick
	# regime's (SPEC-p4e.md S1, S3.6): ticks, empty and single-element ticks and
	# doorbells the tick absorbed per second, both pair queues' overruns and the
	# switched ones among them, and prio_violations, which must be 0 in every row.
	# 0 on a build without the counter.  No count-threshold row: the specification
	# dropped count thresholds, so kwq_wake_count_s is gone with them.
	printf '%s\n%s\n' "$2" "$3" | awk -v dt="$4" 'NR == 1 { for (i = 1; i <= 26; i++) a[i] = $i }
	    NR == 2 { for (i = 1; i <= 26; i++) d[i] = $i - a[i]; ep = d[8] + d[10]; ns = d[9] + d[11];
	        printf("kwq_switched_s=%d kwq_yields_s=%d kwq_tick_yields_s=%d kwq_dem_s=%d kwq_wakeups_s=%d kwq_items_per_pass=%.2f kwq_wake_ep_s=%d kwq_wake_ep_us=%.1f kwq_wakes_low_s=%d kwq_wake_deadline_s=%d kwq_db_batched_s=%d kwq_db_flushed_s=%d kwq_wake_idlecpu_s=%d kwq_capy_s=%d kwq_cuts_s=%d kwq_slice_s=%d kwq_ticks_s=%d kwq_ticks_empty_s=%d kwq_ticks_single_s=%d kwq_db_skipped_s=%d kwq_overruns_s=%d kwq_overruns_switched_s=%d kwq_prio_violations=%d", d[1] / dt, d[2] / dt, d[3] / dt, d[4] / dt, d[5] / dt, d[6] > 0 ? d[7] / d[6] : 0, ep / dt, ep > 0 ? ns / ep / 1000 : 0, d[10] / dt, d[13] / dt, d[14] / dt, d[15] / dt, d[16] / dt, d[17] / dt, d[18] / dt, d[19] / dt, d[20] / dt, d[21] / dt, d[22] / dt, d[23] / dt, d[24] / dt, d[25] / dt, d[26]) }' > "$OUT/kwqrows"
	_ml=$(( $(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.maxlat_ns") > $(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.maxlat_ns") ? $(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.maxlat_ns") : $(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.maxlat_ns") ))
	for kv in $(cat "$OUT/kwqrows") "kwq_maxlat_us=$((_ml / 1000))"; do row "$1" "${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP $1 $(cat "$OUT/kwqrows") kwq_maxlat_us=$((_ml / 1000))"
}

# A competitor pinned to W beside the flow for the P4d tests; sets KPID.
competitor_bg() {	# secs
	case $COMPETITOR in
	intr) if [ "$KIND" = kwq ]; then intr_stress "$1"; return; fi
	      log "WARNING: COMPETITOR=intr on a tq stage: using timer"
	      cpuset -l "$WCPU" "$VICTIM" timer "$1" 100 > "$OUT/competitor" 2>&1 & ;;
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
nivcsw() {	# pid -> its involuntary context switches so far (procstat -r), 0 if gone
	procstat -r "$1" 2>/dev/null | awk '/involuntary context switches/ { print $NF; f = 1 } END { if (!f) print 0 }'
}
q_items() {	# pair-a pair-b cpu -> items overruns debts parks overruns_switched summed over both queues
	for f in items overruns debts parks overruns_switched; do
		echo $(( $(oid0 "kern.kwq.net.queue.$1.cpu.$3.$f") + $(oid0 "kern.kwq.net.queue.$2.cpu.$3.$f") ))
	done | tr '\n' ' '
}
vmstat_total() { vmstat -i | awk '/^Total/ { print $2 }'; }
# Timer interrupts: on amd64 vmstat -i has one cpuN:timer line per CPU; on
# arm64 the generic timer is one line for all CPUs, so the per-CPU count
# comes from dtrace (fbt::handleevents:entry by cpu) when available.
tmr_snap() {	# -> "W's timer interrupts, all CPUs' timer interrupts" (amd64) or "- -"
	if [ "$ARCH" = amd64 ]; then
		vmstat -i | awk -v w="cpu$WCPU:timer" '$1 ~ /^cpu[0-9]+:timer$/ { t += $2; if ($1 == w) ww = $2 } END { print ww + 0, t + 0 }'
	else
		echo "- -"
	fi
}
tmr_bg() {	# secs file: the arm64 substitute, a dtrace count per CPU; sets TPID
	TPID=""
	[ "$ARCH" != amd64 ] && [ "$HAVE_DTRACE" = yes ] || return 0
	dtrace -qn "fbt::handleevents:entry { @c[cpu] = count(); } tick-${1}s { printa(\"cpu%d %@d\\n\", @c); exit(0); }" -o "$2" > /dev/null 2>&1 &
	TPID=$!
}
tmr_rows() {	# TEST "w0 t0" "w1 t1" file secs: intr_w_timer_s and intr_timer_s rows
	if [ "$ARCH" = amd64 ]; then
		_w0=${2%% *}; _t0=${2##* }; _w1=${3%% *}; _t1=${3##* }
		row "$1" intr_w_timer_s $(( (_w1 - _w0) / $5 )); row "$1" intr_timer_s $(( (_t1 - _t0) / $5 ))
		return 0
	fi
	[ -n "${TPID:-}" ] && { wait $TPID 2>/dev/null; TPID=""; }
	[ -f "$4" ] || return 0
	row "$1" intr_w_timer_s "$(awk -v w="cpu$WCPU" -v dt="$5" '$1 == w { print int($2 / dt) }' "$4")"
	row "$1" intr_timer_s "$(awk -v dt="$5" '/^cpu/ { t += $2 } END { print int(t / dt) }' "$4")"
}
intr_stress() {	# secs: start the intr_load stressor of the battery's test module on W; sets KPID
	kldstat -q -m kwq_test || fail "COMPETITOR=intr: kwq_test.ko is not loaded (KWQ_TEST_KO)"
	# intr_secs starts the stressor, so the CPU, period and work come first
	must "intr_load" sysctl -q kern.kwq_test.intr_cpu="$WCPU" kern.kwq_test.intr_us=100 kern.kwq_test.intr_work_ns=20000 kern.kwq_test.intr_secs="$1"
	sleep "$1" &
	KPID=$!
}
intr_stress_stop() { sysctl -q kern.kwq_test.intr_cpu=-1 >/dev/null 2>&1; }
cp_sys_all() {	# -> the machine-wide "user nice sys intr idle" sums
	sysctl -n kern.cp_times | awk '{ for (i = 1; i <= NF; i += 5) { u += $i; n += $(i+1); s += $(i+2); q += $(i+3); d += $(i+4) } print u, n, s, q, d }'
}
kwq_all() {	# -> "wakeups passes ticks" summed over every NET worker, and the pair's items over every CPU
	_s=$(sysctl kern.kwq.net.cpu 2>/dev/null | awk -F'[.:]' '$6 == "wakeups" { w += $NF } $6 == "passes" { p += $NF } $6 == "ticks" { t += $NF } END { print w + 0, p + 0, t + 0 }')
	_i=$(sysctl kern.kwq.net.queue 2>/dev/null | awk -F'[.:]' -v a="$PAIRA" -v b="$PAIRB" '($5 == a || $5 == b) && $8 == "items" { s += $NF } END { print s + 0 }')
	echo "$_s $_i"
}

t_tput1() {
	c0=$(cp_times "$WCPU"); k0=$(kwq_passes); q0=$(kwq_counters "$WCPU"); cport_next; iperf_bg 1 "$CPORT" "$OTHERS"
	gen_sample "$IPID" "$SPID" "$OUT/gen.$STAGE.$REP" & _gp=$!
	wait $IPID; c1=$(cp_times "$WCPU"); k1=$(kwq_passes); q1=$(kwq_counters "$WCPU")
	wait $_gp; read -r gc gs < "$OUT/gen.$STAGE.$REP" || { gc=-1; gs=-1; }
	r=$(iperf_rate); sh=$(cp_shares "$c0" "$c1")
	[ "$KIND" = kwq ] && verify_w kwq "$k0" "$k1"
	kwq_rows tput1 "$q0" "$q1" "$SECS"
	row tput1 gbit "$r"; for kv in $sh; do row tput1 "w_${kv%%=*}" "${kv#*=}"; done
	row tput1 cli_wcore "$gc"; row tput1 srv_wcore "$gs"
	log "$STAGE rep $REP tput1 gbit=$r W=$WCPU $sh cli_wcore=$gc srv_wcore=$gs"
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
	i0=$(vmstat_total); t0=$(tmr_snap); tmr_bg "$SECS" "$OUT/tmr.$STAGE.$REP.machine"; iperf_bg "$LOADCONNS" "" ""; sleep 2
	v=$("$VICTIM" timer $((SECS - 3)) 100); wait $IPID; r=$(iperf_rate); i1=$(vmstat_total); t1=$(tmr_snap)
	for kv in $v; do case $kv in late_p99=*|late_max=*) row machine "${kv%%=*}" "${kv#*=}";; esac; done
	row machine gbit "$r"; row machine intr_s $(( (i1 - i0) / SECS ))
	tmr_rows machine "$t0" "$t1" "$OUT/tmr.$STAGE.$REP.machine" "$SECS"
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
	set -- $CPORTS2; _cp=$1; shift; CPORTS2="$* $_cp"
	k0=$(kwq_passes); q0=$(kwq_counters "$WCPU"); i0=$(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.items"); j0=$(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.items")
	a0=$(q_items "$PAIRA" "$PAIRB" "$WCPU"); n0=$(nivcsw "$_s2")
	IPERF=$OUT/iperf.$STAGE.$REP.coloc
	cpuset -l "$OTHERS" jexec "$JC" iperf3 -f g -c $SRV -p $TCPPORT2 -P 1 --cport "$_cp" -t "$SECS" > "$IPERF" 2>&1
	k1=$(kwq_passes); q1=$(kwq_counters "$WCPU"); i1=$(oid0 "kern.kwq.net.queue.$PAIRA.cpu.$WCPU.items"); j1=$(oid0 "kern.kwq.net.queue.$PAIRB.cpu.$WCPU.items")
	a1=$(q_items "$PAIRA" "$PAIRB" "$WCPU"); n1=$(nivcsw "$_s2")
	kill $_s2 2>/dev/null; wait $_s2 2>/dev/null
	printf '%s\n%s\n' "$a0" "$a1" | awk -v dt="$SECS" 'NR == 1 { for (i = 1; i <= 5; i++) a[i] = $i } NR == 2 { printf("overruns=%d overruns_switched=%d debts=%d parks=%d", $2 - a[2], $5 - a[5], $3 - a[3], $4 - a[4]) }' > "$OUT/colocq"
	for kv in $(cat "$OUT/colocq"); do row coloc "${kv%%=*}" "${kv#*=}"; done
	row coloc rcv_nivcsw_s $(( (n1 - n0) / SECS ))
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
	d=$(printf '%s\n%s\n%s\n%s\n' "$a0" "$a1" "$b0" "$b1" | awk 'NR == 1 { for (i = 1; i <= 5; i++) a[i] = $i } NR == 2 { for (i = 1; i <= 5; i++) da[i] = $i - a[i] } NR == 3 { for (i = 1; i <= 5; i++) b[i] = $i } NR == 4 { for (i = 1; i <= 5; i++) db[i] = $i - b[i]; printf("q1_items=%d q1_overruns=%d q1_overruns_switched=%d q1_debts=%d q1_parks=%d q2_items=%d q2_overruns=%d q2_overruns_switched=%d q2_debts=%d q2_parks=%d share1=%.3f", da[1], da[2], da[5], da[3], da[4], db[1], db[2], db[5], db[3], db[4], (da[1] + db[1]) > 0 ? da[1] / (da[1] + db[1]) : 0) }')
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

# ---- SPEC-p4e tests
t_sparse() {
	[ -n "${SEED:-}" ] || { log "WARNING: sparse needs the module's hash seed (victim seed) to spread the flows; skipped"; return 0; }
	for n in $SPARSE_N; do
		_cap=$((NCPU * 4)); [ "$n" -gt "$_cap" ] && n=$_cap
		sp=$("$VICTIM" uports "$SEED" "$NCPU" $CLI $SRV $UDPPORT "$n") || { log "WARNING: sparse: no $n source ports"; continue; }
		for r in $SPARSE_RATE; do
			t=sparse_${n}_${r}; gap=$((1000000 / r))
			c0=$(cp_sys_all); w0=$(kwq_all); i0=$(vmstat_total); t0=$(tmr_snap); tmr_bg "$UDPSECS" "$OUT/tmr.$STAGE.$REP.$t"
			rm -f "$OUT/sparse.$STAGE.$REP.$t"; _pids=""
			for p in $sp; do
				cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT $UDPSECS "$p" "$gap" >> "$OUT/sparse.$STAGE.$REP.$t" 2>&1 &
				_pids="$_pids $!"
			done
			# shellcheck disable=SC2086
			wait $_pids
			c1=$(cp_sys_all); w1=$(kwq_all); i1=$(vmstat_total); t1=$(tmr_snap)
			st=$(awk '/^udp n=/ { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "rtt_p50") p50[n] = kv[2]; if (kv[1] == "rtt_p99") p99[n] = kv[2]; if (kv[1] == "lost") lost += kv[2] } n++ }
			    function med(a, n,   i, j, t) { for (i = 1; i < n; i++) for (j = i; j > 0 && a[j-1] > a[j]; j--) { t = a[j]; a[j] = a[j-1]; a[j-1] = t } return n ? a[int(n / 2)] : 0 }
			    function mn(a, n,   i, m) { m = a[0]; for (i = 1; i < n; i++) if (a[i] < m) m = a[i]; return n ? m : 0 }
			    function mx(a, n,   i, m) { m = a[0]; for (i = 1; i < n; i++) if (a[i] > m) m = a[i]; return n ? m : 0 }
			    END { printf("flows=%d rtt_p50_min=%d rtt_p50_med=%d rtt_p50_max=%d rtt_p99_min=%d rtt_p99_med=%d rtt_p99_max=%d lost=%d", n, mn(p50, n), med(p50, n), mx(p50, n), mn(p99, n), med(p99, n), mx(p99, n), lost) }' "$OUT/sparse.$STAGE.$REP.$t")
			kw=$(printf '%s\n%s\n' "$w0" "$w1" | awk -v dt="$UDPSECS" 'NR == 1 { for (i = 1; i <= 4; i++) a[i] = $i } NR == 2 { w = $1 - a[1]; p = $2 - a[2]; k = $3 - a[3]; it = $4 - a[4]; printf("kwq_wakeups_s=%d kwq_ticks_s=%d kwq_passes_s=%d kwq_items_per_pass=%.2f", w / dt, k / dt, p / dt, p > 0 ? it / p : 0) }')
			sys=$(printf '%s\n%s\n' "$c0" "$c1" | awk 'NR == 1 { for (i = 1; i <= 5; i++) a[i] = $i } NR == 2 { t = 0; for (i = 1; i <= 5; i++) { d[i] = $i - a[i]; t += d[i] } if (t <= 0) t = 1; printf("sys_share=%.3f", (d[3] + d[4]) / t) }')
			for kv in $st $kw $sys; do row "$t" "${kv%%=*}" "${kv#*=}"; done
			row "$t" intr_s $(( (i1 - i0) / UDPSECS ))
			tmr_rows "$t" "$t0" "$t1" "$OUT/tmr.$STAGE.$REP.$t" "$UDPSECS"
			log "$STAGE rep $REP $t $st $kw $sys intr_s=$(( (i1 - i0) / UDPSECS ))"
		done
	done
}
udp_burst_run() {	# TESTNAME: one udpburst per B:G of UDP_BURSTS, counters per burst
	for bg in $UDP_BURSTS; do
		B=${bg%%:*}; G=${bg#*:}; t=${1}_${B}_${G}
		q0=$(kwq_counters "$WCPU")
		u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udpburst $SRV $UDPPORT $UDPSECS "$UPORT" "$B" "$G")
		q1=$(kwq_counters "$WCPU")
		nb=$(echo "$u" | sed -n 's/.*bursts=\([0-9]*\).*/\1/p'); [ -n "$nb" ] && [ "$nb" -gt 0 ] || nb=1
		for kv in $u; do case $kv in first_*|last_*|rtt_p99=*|lost=*|bursts=*) row "$t" "${kv%%=*}" "${kv#*=}";; esac; done
		[ "$KIND" = kwq ] && printf '%s\n%s\n' "$q0" "$q1" | awk -v nb="$nb" 'NR == 1 { for (i = 1; i <= 26; i++) a[i] = $i } NR == 2 { printf("wakeups_per_burst=%.2f ticks_per_burst=%.2f ticks_empty_per_burst=%.2f passes_per_burst=%.2f", ($5 - a[5]) / nb, ($20 - a[20]) / nb, ($21 - a[21]) / nb, ($6 - a[6]) / nb) }' > "$OUT/burstrows"
		[ "$KIND" = kwq ] && for kv in $(cat "$OUT/burstrows"); do row "$t" "${kv%%=*}" "${kv#*=}"; done
		log "$STAGE rep $REP $t $u $([ "$KIND" = kwq ] && cat "$OUT/burstrows")"
	done
}
t_udp_burst() { udp_burst_run udp_burst; }
t_udp_burst_load() {
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	udp_burst_run udp_burst_load
	wait $IPID; r=$(iperf_rate); row udp_burst_load gbit "$r"
}

battery() {
	case "${SMT_SIBLINGS:-share}" in
	exclude) OTHERS=$(others_than_core "$WCPU") ;;
	share) OTHERS=$(others_than "$WCPU") ;;
	*) fail "SMT_SIBLINGS must be share or exclude, not $SMT_SIBLINGS" ;;
	esac
	log "$STAGE: load generators pinned to CPUs $OTHERS (SMT_SIBLINGS=${SMT_SIBLINGS:-share}, W's core $(smt_siblings "$WCPU"))"
	cpuset -l "$OTHERS" -p "$SPID"		# the iperf3 server, off W
	cpuset -l "$OTHERS" -p "$EPID"
	for t in $TESTS; do
		TEST=$t	# also the iperf3 report's name; lib.sh's TEST only prefixed the log
		case $t in
		tput1|tput16|udp_idle|udp_load|spin|timer|exec|machine|preempt|coloc|rwlock|xcpu|twoq|churn|magnet|fair|sparse|udp_burst|udp_burst_load) t_$t ;;
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
