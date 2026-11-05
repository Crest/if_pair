#!/bin/sh
# where2_lab.sh - hardware counters and data waits behind kwq-geli's write
# gap on lab: geli_bench scale at 32 and 128 providers, REPS=1, for stock
# GELI (eli), kwq-geli at the defaults (kwqshard) and with BULK's share
# limit off (nocap: kern.kwq.bulk.cap_permille=1000, restored after).
# During each bulk phase, 8 s from 1 s after iolat -w starts, pmcstat
# counts cycles, instructions and L1D fills from DRAM (demand, hardware
# prefetch, any) and from L3 on every CPU, per second; waits.d times the
# data's waits around it.  hwpmc must be loaded (logged by the caller);
# dtraceall is loaded and unloaded here.  NO_PMC=1 skips the counters,
# WHERE2_OUT names the output directory (where2).  sh where2_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out/${WHERE2_OUT:-where2}; mkdir -p $O; rm -f $D/where2.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) where2: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -n "${NO_PMC:-}" ] || kldstat -q -m hwpmc || { log "hwpmc not loaded: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/geom_kwqeli.ko || { log "cannot load kwq, geom_kwqeli"; exit 1; }
dl=0; kldstat -q -m dtraceall || { sudo -n kldload dtraceall && dl=1 && log "loaded dtraceall"; }
c0=$(sysctl -n kern.kwq.bulk.cap_permille)
EV="-s ls_not_halted_cyc -s ex_ret_instr -s ls_dmnd_fills_from_sys.mem_io_local -s ls_hw_pf_dc_fills.mem_io_local -s ls_any_fills_from_sys.mem_io_local -s ls_any_fills_from_sys.int_cache"
watch() {	# TAG: pmcstat over each bulk phase
	for n in 32 128; do
		while ! pgrep -f "iolat -w -b 1048576" >/dev/null; do sleep 0.1; done
		sleep 1
		sudo -n pmcstat -w 1 $EV -o $O/pmc_$1_$n.txt sleep 8
		while pgrep -f "iolat -w -b 1048576" >/dev/null; do sleep 0.1; done
	done
}
for run in eli:eli kwqshard:kwqshard nocap:kwqshard; do
	tag=${run%%:*}; st=${run#*:}; mod=geom_kwqeli; [ $st = eli ] && mod=geom_eli
	if [ $tag = nocap ]; then sudo -n sysctl -q kern.kwq.bulk.cap_permille=1000 >/dev/null; log "set kern.kwq.bulk.cap_permille=1000 (was $c0)"; fi
	cat > $O/run_$tag.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_$tag.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="32 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $st > "$O/scale_$tag.log" 2>&1
EOS
	wpid=; [ -n "${NO_PMC:-}" ] || { watch $tag & wpid=$!; }
	sudo -n dtrace -s $D/waits.d $mod -c "sh $O/run_$tag.sh" > $O/waits_$tag.out 2>&1
	log "waits.d over $tag: exit $?"
	[ -n "$wpid" ] && { kill $wpid 2>/dev/null; wait $wpid 2>/dev/null; }
	if [ $tag = nocap ]; then sudo -n sysctl -q kern.kwq.bulk.cap_permille=$c0 >/dev/null; log "restored kern.kwq.bulk.cap_permille=$c0"; fi
done
[ $dl = 1 ] && sudo -n kldunload dtraceall && log "unloaded dtraceall"
sudo -n kldunload geom_kwqeli; sudo -n kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|dtrace'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/where2.done
