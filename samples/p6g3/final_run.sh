#!/bin/sh
# final_run.sh - after G2, the code at its defaults (place=2, directed=2,
# directed_us=10; b70e3db) against today's code (geli_bench's kwq stage,
# place=0) on one boot of lab or a07.  sh final_run.sh lab|a07 DIR
#   geli_bench scale (lab 4 16 128, a07 4 16 64 128) on kwqnosteal, kwq,
#   kwqshard; tput and resp on kwq, kwqshard; on a07 also ssd (16 SSD
#   models, seqread seqwrite randread) and the randread A/B: ten blocks of
#   three runs of kwq and three of kwqshard, with the BULK workers' steal
#   counters per block; then kwq_crypto K4 (1 and 4 stealable queues fed on
#   CPU 1, 16 x 64 KiB in flight), today and P8, 3 repetitions.
# kwq.ko is loaded once at its defaults and no knob is set but place.
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/final.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log; NS="4 16 128"
	log() { echo "$(date -u +%FT%TZ) final: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log; NS="4 16 64 128"
	log() { echo "$(date -u +%FT%TZ) final: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
$SU kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
dflt="place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us)"
log "loaded kwq.ko at its defaults: $dflt"
[ "$dflt" = "place=2 directed=2 directed_us=10" ] || { log "not the G2 defaults: abort"; $SU kldunload kwq; exit 1; }
sums() {
	sysctl kern.kwq.bulk.cpu | awk -F': ' '
	    /\.steal_wakes:/ {w += $2} /\.steals:/ {s += $2} /\.directed:/ {d += $2} /\.chained:/ {c += $2}
	    END {printf "%d %d %d %d\n", w, s, d, c}'
}
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_final.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS="$NS" SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" kwqnosteal kwq kwqshard > "$O/scale_final.log" 2>&1
log "geli_bench scale exit $?"
$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/tput_final.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 TESTS="tput resp" sh "$B" kwq kwqshard > "$O/tput_final.log" 2>&1
log "geli_bench tput resp exit $?"
if [ $M = a07 ]; then
	$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssd_final.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 TESTS=ssd \
	    sh "$B" kwq kwqshard > "$O/ssd_final.log" 2>&1
	log "geli_bench ssd exit $?"
	: > $O/ssdab_final_counters.txt
	for i in 1 2 3 4 5 6 7 8 9 10; do
		c0=$(sums)
		$SU env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssdab_final_$i.tsv" SETTINGS_LOG="$L" \
		    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 SSD_LOADS=randread TESTS=ssd \
		    sh "$B" kwq kwqshard > "$O/ssdab_final_$i.log" 2>&1
		r=$?
		c1=$(sums)
		echo $c0 $c1 | awk -v b=$i '{printf "block %d wakes %d steals %d directed %d chained %d\n", b, $5-$1, $6-$2, $7-$3, $8-$4}' >> $O/ssdab_final_counters.txt
		log "randread block $i exit $r"
	done
fi
$SU kldload $S/kwq_crypto.ko || { log "cannot load kwq_crypto"; $SU kldunload kwq; exit 1; }
K=$O/k4_final.tsv; : > $K
for rep in 1 2 3; do
	for cfg in today:0 p8:2; do
		name=${cfg%%:*}; $SU sysctl -q kern.kwq.bulk.place=${cfg#*:} >/dev/null
		for nq in 1 4; do
			$SU sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=$nq kern.kwq_crypto.depth=16 \
			    kern.kwq_crypto.cpu=1 kern.kwq_crypto.stealable=1 kern.kwq_crypto.secs=3 \
			    kern.kwq_crypto.verify=0 kern.kwq_crypto.bytes=65536 >/dev/null
			b=$(sums)
			$SU sysctl -q kern.kwq_crypto.run=1 >/dev/null
			while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
			a=$(sums)
			c=$(echo $b $a | awk '{printf "wakes %d steals %d directed %d chained %d", $5-$1, $6-$2, $7-$3, $8-$4}')
			printf '%s\t%s\t%s\t%s\t%s\n' $name $nq $rep "$(dmesg | grep 'kwq_crypto: kwq nq' | tail -1)" "$c" >> $K
		done
	done
done
$SU sysctl -q kern.kwq.bulk.place=2 >/dev/null
$SU kldunload kwq_crypto; $SU kldunload kwq
log "unloaded kwq_crypto, kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $($SU mdconfig -l | wc -w), uptime $(uptime)"
touch $D/final.done
