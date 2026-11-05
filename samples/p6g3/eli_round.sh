#!/bin/sh
# eli_round.sh - stock GELI (n providers x m bound workers) against
# kwq-geli under today's placement and under P8 at the G2 defaults, one
# boot, stages interleaved by geli_bench.  sh eli_round.sh lab|a07 DIR
#   scale (lab 4 16 32 64 128, a07 4 16 64 128 providers) on eli, kwq,
#   kwqshard; tput and resp on eli, kwq, kwqshard; on a07 also ssd (16
#   SSD models, three loads) on nop, eli, kwq, kwqshard.  3 repetitions.
# kwq.ko is loaded once at its defaults; geom_eli stays loaded (swap).
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/eli.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 32 64 128"
	log() { echo "$(date -u +%FT%TZ) eli: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) eli: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime), processes $(ps -ax | wc -l)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
[ -z "$($SU geli list 2>/dev/null | grep '^Geom name: md')" ] || { log "geli providers on md exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
$SU kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
dflt="place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us)"
log "loaded kwq.ko at its defaults: $dflt"
[ "$dflt" = "place=2 directed=2 directed_us=10" ] || { log "not the G2 defaults: abort"; $SU kldunload kwq; exit 1; }
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_eli.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" eli kwq kwqshard > "$O/scale_eli.log" 2>&1
log "geli_bench scale exit $?, processes $(ps -ax | wc -l)"
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_eli.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 TESTS="tput resp" sh "$B" eli kwq kwqshard > "$O/tput_eli.log" 2>&1
log "geli_bench tput resp exit $?"
if [ $M = a07 ]; then
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssd_eli.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 TESTS=ssd \
	    sh "$B" nop eli kwq kwqshard > "$O/ssd_eli.log" 2>&1
	log "geli_bench ssd exit $?"
fi
$SU kldunload kwq
log "unloaded kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), geli on md $($SU geli list 2>/dev/null | grep -c '^Geom name: md'), processes $(ps -ax | wc -l), uptime $(uptime)"
touch $D/eli.done
