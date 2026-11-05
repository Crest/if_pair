#!/bin/sh
# k2ab.sh - P6g.3 K2 A/B on lab: geli_bench scale at 128 providers on kwq
# (today) and kwqshard (P8), shard.directed 0, 2, 0, 2 in turn, kwq.ko
# preloaded so the knob holds across the stages.  sh k2ab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/k2ab.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) k2ab: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko"
i=0
for d in 0 2 0 2; do
	i=$((i + 1))
	sudo -n sysctl -q kern.kwq.bulk.shard.directed=$d >/dev/null
	log "block $i: set shard.directed=$d"
	sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/k2ab_d${d}_$i.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS=128 SCALE_MDSIZE=32 TESTS=scale \
	    sh "$B" kwq kwqshard > "$O/k2ab_d${d}_$i.log" 2>&1
	log "geli_bench scale exit $?"
done
sudo -n sysctl -q kern.kwq.bulk.place=0 kern.kwq.bulk.shard.directed=0 >/dev/null
sudo -n kldunload kwq
log "restored the knobs, unloaded kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/k2ab.done
