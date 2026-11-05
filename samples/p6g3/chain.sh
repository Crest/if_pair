#!/bin/sh
# chain.sh - P6g.3 round 6 (lab or a07): geli_bench under P8 with
# shard.directed=2 (the chain) against today and no stealing, kwq.ko
# preloaded so the knob holds across geli_bench's stages: scale on
# kwqnosteal, kwq, kwqshard; tput and resp on kwq, kwqshard; on a07 also
# ssd (16 gnop SSD models) on kwq, kwqshard.  sh chain.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/chain.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 128"
	log() { echo "$(date -u +%FT%TZ) chain: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) chain: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
$SU kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
$SU sysctl -q kern.kwq.bulk.shard.directed=2 >/dev/null
log "loaded kwq.ko, set shard.directed=2"
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_chain.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" kwqnosteal kwq kwqshard > "$O/scale_chain.log" 2>&1
log "geli_bench scale exit $?"
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_chain.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 TESTS="tput resp" sh "$B" kwq kwqshard > "$O/tput_chain.log" 2>&1
log "geli_bench tput resp exit $?"
if [ $M = a07 ]; then
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssd_chain.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 TESTS=ssd \
	    sh "$B" kwq kwqshard > "$O/ssd_chain.log" 2>&1
	log "geli_bench ssd exit $?"
fi
$SU sysctl -q kern.kwq.bulk.place=0 kern.kwq.bulk.shard.directed=0 >/dev/null
$SU kldunload kwq
log "restored the knobs, unloaded kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), uptime $(uptime)"
touch $D/chain.done
