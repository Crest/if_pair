#!/bin/sh
# netprof.sh lab DIR STAGE...: a dtrace profile of the kernel's leaf functions
# in the dense phase of NET beside GELI writes, one run per stage, into
# DIR/diag/netprof_<stage>.txt.  dtrace stays loaded for the runs, its start
# taking seconds, and is unloaded again if it was not loaded before.
M=${1:?lab}; D=${2:?DIR}; shift 2; O=$D/diag; mkdir -p $O
SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) netprof: $*" | tee -a "$L"; }
DT=
if ! kldstat -q -m dtrace; then
	$SU kldload dtraceall && DT=1 && log "loaded dtraceall"
fi
for st in "$@"; do
	OUT=out8/$st NET_STAGES=$st NET_REPS=1 NET_SECS=25 sh $D/src/samples/p6g11/nettime.sh $M $D > $O/netprof_$st.log 2>&1 &
	n=$!
	i=0
	until pgrep -f 'iperf3 -f g -c' > /dev/null || [ $i -gt 600 ]; do sleep 0.5; i=$((i + 1)); done
	sleep 2
	$SU dtrace -q -x aggsize=8m -n 'profile-997 /arg0/ { @f[func(arg0)] = count(); } profile-997 /arg1/ { @u = count(); } tick-6s { printa("%@8u %a\n", @f); printa("%@8u user\n", @u); exit(0); }' 2>&1 | sort -n | tail -32 > $O/netprof_$st.txt
	vmstat -z | grep -E '^(ITEM|GELI)' >> $O/netprof_$st.txt
	wait $n
	log "$st done: $(tail -1 $O/netprof_$st.log | cut -c1-90)"
done
if [ -n "$DT" ]; then
	$SU kldunload dtraceall && $SU kldunload profile dtnfscl dtmalloc dtaudit fbt sdt systrace systrace_freebsd32 systrace_linux systrace_linux32 fasttrap dtrace_test 2>/dev/null
	log "unloaded dtraceall, dtrace modules left: $(kldstat | grep -cE 'dtrace|dtmalloc|dtnfscl|fbt|sdt|systrace|profile|fasttrap|opensolaris')"
fi
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|simdisk|kpair')"
