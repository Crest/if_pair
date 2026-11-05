#!/bin/sh
# ssdab_a07.sh - P6g.3 on a07: geli_bench ssd randread (16 gnop SSD models)
# on kwq (today) and kwqshard (P8), shard.directed 0, 2, 0, 2 in turn, kwq.ko
# preloaded so the knob holds across the stages.  sh ssdab_a07.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/ssdab.done
L=$HOME/if_pair-scratch/settings_a07.log
log() { echo "$(date -u +%FT%TZ) ssdab: $*" | doas -n tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(doas -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
doas -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko"
i=0
for d in 0 2 0 2; do
	i=$((i + 1))
	doas -n sysctl -q kern.kwq.bulk.shard.directed=$d >/dev/null
	log "block $i: set shard.directed=$d"
	doas -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssdab_d${d}_$i.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 SSD_LOADS=randread TESTS=ssd \
	    sh "$B" kwq kwqshard > "$O/ssdab_d${d}_$i.log" 2>&1
	log "geli_bench ssd exit $?"
done
doas -n sysctl -q kern.kwq.bulk.place=0 kern.kwq.bulk.shard.directed=0 >/dev/null
doas -n kldunload kwq
log "restored the knobs, unloaded kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(doas -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/ssdab.done
