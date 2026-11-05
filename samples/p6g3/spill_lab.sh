#!/bin/sh
# spill_lab.sh - P6g.3 round 2 on lab: geli_bench scale and tput under P8
# with shard.spill_idle=1 (and donor_us 500, then 50) against today, kwq.ko
# preloaded so the knobs hold across geli_bench's stages.  sh spill_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/spill.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) spill: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
for du in 500 50; do
	sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
	sudo -n sysctl -q kern.kwq.bulk.shard.spill_idle=1 kern.kwq.bulk.shard.donor_us=$du >/dev/null
	log "loaded kwq.ko, set shard.spill_idle=1 shard.donor_us=$du"
	sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_spill$du.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="4 16 128" SCALE_MDSIZE=32 TESTS=scale \
	    sh "$B" kwq kwqshard > "$O/scale_spill$du.log" 2>&1
	log "geli_bench scale exit $?"
	sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_spill$du.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 TESTS="tput resp" sh "$B" kwq kwqshard > "$O/tput_spill$du.log" 2>&1
	log "geli_bench tput resp exit $?"
	sudo -n sysctl -q kern.kwq.bulk.place=0 kern.kwq.bulk.shard.spill_idle=0 kern.kwq.bulk.shard.donor_us=500 >/dev/null
	sudo -n kldunload kwq
	log "restored the knobs, unloaded kwq"
done
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/spill.done
