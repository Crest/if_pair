#!/bin/sh
# prio_sweep.sh - which BULK priority, on lab, with budget_sectors 0
# (geli_bench's kwqbio): 2 today's; 41 above md(4) but past ULE's
# preempt_thresh (40), so ranked first without preempting; 43 md's own;
# 44 just below md; 55 the last kernel priority.  Stock GELI is not run
# here (steps.sh has it).  scale at 32 and 128 providers and tput, 3
# repetitions, priorities interleaved.
# PRIOS and SWEEP_OUT override the priorities and the output directory.
# Output: DIR/out/sweep/p<priority>/{scale,tput}_r<rep>.tsv.  sh prio_sweep.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out/${SWEEP_OUT:-sweep}; rm -f $D/sweep.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) sweep: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
r=1
while [ $r -le 3 ]; do
	for p in ${PRIOS:-2 41 43 44 55}; do
		o=$O/p$p; mkdir -p $o
		[ $p = 2 ] || { sudo -n kenv kern.kwq.bulk.priority=$p >/dev/null; log "set kenv kern.kwq.bulk.priority=$p"; }
		sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
		got=$(sysctl -n kern.kwq.bulk.priority)
		[ "$got" = $p ] || { log "priority $got, wanted $p: abort"; sudo -n kldunload kwq; sudo -n kenv -u kern.kwq.bulk.priority; exit 1; }
		sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/scale_r$r.tsv" SETTINGS_LOG="$L" \
		    REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="32 128" SCALE_MDSIZE=32 TESTS=scale \
		    sh "$B" kwqbio > "$o/scale_r$r.log" 2>&1
		e1=$?
		sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$o/tput_r$r.tsv" SETTINGS_LOG="$L" \
		    REPS=1 TESTS=tput sh "$B" kwqbio > "$o/tput_r$r.log" 2>&1
		e2=$?
		sudo -n kldunload kwq
		[ $p = 2 ] || { sudo -n kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
		log "rep $r priority $p: exit $e1 $e2"
		[ "$e1$e2" = 00 ] || { log "a battery failed: abort"; exit 1; }
	done
	r=$((r + 1))
done
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(sudo -n mdconfig -l | wc -w), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), uptime $(uptime)"
touch $D/sweep.done
