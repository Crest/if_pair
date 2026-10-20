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
# Requires iperf3 and cc (victim.c is built on the fly).  Runs as root.
. "$(dirname "$0")/lib.sh"

command -v iperf3 >/dev/null 2>&1 || fail "iperf3 not installed"
command -v cc >/dev/null 2>&1 || fail "no cc for victim.c"
[ "$(id -u)" -eq 0 ] || fail "must run as root"
[ -n "$1" ] && [ -f "$1" ] || fail "usage: compare.sh STAGEFILE"
[ -n "${OUT_TSV:-}" ] || fail "set OUT_TSV to the result file"
STAGEFILE=$1

REPS=${REPS:-1}
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
	sleep 0.5
}
stage_down() {
	kill $UPID $SPID 2>/dev/null; wait $UPID $SPID 2>/dev/null
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
		# six TCP ports: one per run that uses the single flow, so no run
		# meets the previous run's TIME_WAIT entry ("unable to connect
		# stream: Address already in use", seen on a07 and lab)
		set -- $("$VICTIM" ports "$seed" "$NCPU" "$WCPU" $CLI $SRV $TCPPORT $UDPPORT 6) || fail "no ports for CPU $WCPU"
		CPORTS=$(echo "${1#cports=}" | tr ',' ' '); UPORT=${2#uport=}
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

battery() {
	OTHERS=$(others_than "$WCPU")
	cpuset -l "$OTHERS" -p "$SPID"		# the iperf3 server, off W

	TEST=tput1
	c0=$(cp_times "$WCPU"); k0=$(kwq_passes); q0=$(kwq_counters "$WCPU"); cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; wait $IPID; c1=$(cp_times "$WCPU"); k1=$(kwq_passes); q1=$(kwq_counters "$WCPU")
	r=$(iperf_rate); sh=$(cp_shares "$c0" "$c1")
	[ "$KIND" = kwq ] && verify_w kwq "$k0" "$k1"
	kwq_rows tput1 "$q0" "$q1" "$SECS"
	row tput1 gbit "$r"; for kv in $sh; do row tput1 "w_${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP tput1 gbit=$r W=$WCPU $sh"

	TEST=tput16
	iperf_bg 16 "" ""; wait $IPID; r=$(iperf_rate)
	row tput16 gbit "$r"; log "$STAGE rep $REP tput16 gbit=$r"

	TEST=udp_idle
	u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT $UDPSECS "$UPORT" 500)
	for kv in $u; do case $kv in rtt_*|lost=*) row udp_idle "${kv%%=*}" "${kv#*=}";; esac; done
	log "$STAGE rep $REP udp_idle $u"

	TEST=udp_load
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	q0=$(kwq_counters "$WCPU")
	u=$(cpuset -l "$OTHERS" jexec "$JC" "$VICTIM" udp $SRV $UDPPORT $UDPSECS "$UPORT" 500)
	q1=$(kwq_counters "$WCPU"); wait $IPID; r=$(iperf_rate)
	kwq_rows udp_load "$q0" "$q1" "$UDPSECS"
	for kv in $u; do case $kv in rtt_*|lost=*) row udp_load "${kv%%=*}" "${kv#*=}";; esac; done
	row udp_load gbit "$r"; log "$STAGE rep $REP udp_load $u gbit=$r"

	TEST=spin
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	c0=$(cp_times "$WCPU"); v=$(cpuset -l "$WCPU" "$VICTIM" spin $((SECS - 3))); c1=$(cp_times "$WCPU")
	wait $IPID; r=$(iperf_rate); sh=$(cp_shares "$c0" "$c1")
	s=$(echo "$v" | sed -n 's/.*share=\([0-9.]*\).*/\1/p')
	row spin share "$s"; row spin gbit "$r"; for kv in $sh; do row spin "w_${kv%%=*}" "${kv#*=}"; done
	log "$STAGE rep $REP spin share=$s gbit=$r $sh"

	TEST=timer
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	v=$(cpuset -l "$WCPU" "$VICTIM" timer $((SECS - 3)) 100); wait $IPID; r=$(iperf_rate)
	for kv in $v; do case $kv in late_p99=*|late_max=*|turn_p99=*) row timer "${kv%%=*}" "${kv#*=}";; esac; done
	row timer gbit "$r"; log "$STAGE rep $REP timer $v gbit=$r"

	TEST=exec
	cport_next; iperf_bg 1 "$CPORT" "$OTHERS"; sleep 2
	v=$(cpuset -l "$WCPU" "$VICTIM" exec $(((SECS - 3) * 40)) 20); wait $IPID
	for kv in $v; do case $kv in exec_avg=*|exec_p99=*) row exec "${kv%%=*}" "${kv#*=}";; esac; done
	log "$STAGE rep $REP exec $v"

	TEST=machine
	iperf_bg "$LOADCONNS" "" ""; sleep 2
	v=$("$VICTIM" timer $((SECS - 3)) 100); wait $IPID; r=$(iperf_rate)
	for kv in $v; do case $kv in late_p99=*|late_max=*) row machine "${kv%%=*}" "${kv#*=}";; esac; done
	row machine gbit "$r"; log "$STAGE rep $REP machine $v gbit=$r"
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
