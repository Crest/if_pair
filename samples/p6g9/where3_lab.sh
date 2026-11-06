#!/bin/sh
# where3_lab.sh - why steps.sh's numbers move, on lab, after it: as
# ../p6g8/where2_lab.sh, geli_bench scale at 32 and 128 providers, REPS=1,
# counters over each bulk phase and waits.d around it, for stock GELI (kwq
# unloaded), then at each priority kwq-geli as it is, budget_sectors 0, and
# the share limit off (no cuts and no hand-overs: what budget_sectors 0
# leaves to explain).  Then overrun.d (-L) over 16 and 128 providers at the
# defaults and at budget_sectors 0: what the check's move costs.
# hwpmc and dtraceall are loaded and unloaded here.  sh where3_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out/where3; mkdir -p $O; rm -f $D/where3.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) where3: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
pl=0; kldstat -q -m hwpmc || { sudo -n kldload hwpmc && pl=1 && log "loaded hwpmc"; }
dl=0; kldstat -q -m dtraceall || { sudo -n kldload dtraceall && dl=1 && log "loaded dtraceall"; }
EV="-s ls_not_halted_cyc -s ex_ret_instr -s ls_dmnd_fills_from_sys.mem_io_local -s ls_hw_pf_dc_fills.mem_io_local -s ls_any_fills_from_sys.mem_io_local -s ls_any_fills_from_sys.int_cache"
watch() {	# TAG: pmcstat over each bulk phase
	for n in 32 128; do
		while ! pgrep -f "iolat -w -b 1048576" >/dev/null; do sleep 0.1; done
		sleep 1
		sudo -n pmcstat -w 1 $EV -o $O/pmc_$1_$n.txt sleep 8
		while pgrep -f "iolat -w -b 1048576" >/dev/null; do sleep 0.1; done
	done
}
run() {	# TAG STAGE MODULE
	cat > $O/run_$1.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_$1.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="32 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $2 > "$O/scale_$1.log" 2>&1
EOS
	watch $1 & wpid=$!
	sudo -n dtrace -s $D/waits.d $3 -c "sh $O/run_$1.sh" > $O/waits_$1.out 2>&1
	log "waits.d over $1: exit $?"
	kill $wpid 2>/dev/null; wait $wpid 2>/dev/null
}
over() {	# TAG STAGE
	cat > $O/run_over_$1.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_over_$1.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="16 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $2 > "$O/scale_over_$1.log" 2>&1
EOS
	sudo -n dtrace -L $D/src/kwq/kwq -s $D/overrun.d -c "sh $O/run_over_$1.sh" > $O/overrun_$1.out 2>&1
	log "overrun.d over $1: exit $?"
}
run stock eli geom_eli
for p in default 55; do
	t=p2; [ $p = default ] || { t=p55; sudo -n kenv kern.kwq.bulk.priority=$p >/dev/null; log "set kenv kern.kwq.bulk.priority=$p"; }
	sudo -n kldload $S/kwq.ko && sudo -n kldload $S/geom_kwqeli.ko || { log "cannot load kwq, geom_kwqeli"; break; }
	log "loaded kwq.ko, geom_kwqeli.ko: priority=$(sysctl -n kern.kwq.bulk.priority)"
	c0=$(sysctl -n kern.kwq.bulk.cap_permille)
	run ${t}_base kwqshard geom_kwqeli
	run ${t}_bio kwqbio geom_kwqeli
	sudo -n sysctl -q kern.kwq.bulk.cap_permille=1000 >/dev/null; log "set kern.kwq.bulk.cap_permille=1000 (was $c0)"
	run ${t}_nocap kwqshard geom_kwqeli
	sudo -n sysctl -q kern.kwq.bulk.cap_permille=$c0 >/dev/null; log "restored kern.kwq.bulk.cap_permille=$c0"
	over ${t}_base kwqshard
	over ${t}_bio kwqbio
	sudo -n kldunload geom_kwqeli; sudo -n kldunload kwq; log "unloaded geom_kwqeli, kwq"
	[ $p = default ] || { sudo -n kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
done
[ $dl = 1 ] && sudo -n kldunload dtraceall && log "unloaded dtraceall"
[ $pl = 1 ] && sudo -n kldunload hwpmc && log "unloaded hwpmc"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|dtrace|hwpmc'), md $(sudo -n mdconfig -l | wc -w), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), uptime $(uptime)"
touch $D/where3.done
