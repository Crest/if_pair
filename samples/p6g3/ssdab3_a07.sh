#!/bin/sh
# ssdab3_a07.sh - P6g.3 on a07: geli_bench ssd randread (16 SSD models) on
# kwq (today) and kwqshard (P8), one block with directed 0: the glitch fix's check.
# shard.directed_us): (0,0) (2,0) (2,10) twice, kwq.ko preloaded.  Per
# block the BULK workers' steal counters, and every 2 s the spread of the
# lists' cost_ns (count, min, median, p90, max of the non-zero ones).
#   sh ssdab3_a07.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/ssdab3.done
L=$HOME/if_pair-scratch/settings_a07.log
log() { echo "$(date -u +%FT%TZ) ssdab3: $*" | doas -n tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(doas -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
doas -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko"
sums() {
	sysctl kern.kwq.bulk.cpu | awk -F': ' '
	    /\.steal_wakes:/ {w += $2} /\.steals:/ {s += $2} /\.directed:/ {d += $2} /\.chained:/ {c += $2}
	    END {printf "%d %d %d %d\n", w, s, d, c}'
}
sampler() {	# FILE: until the stop file appears
	while [ ! -f $D/sampler.stop ]; do
		sysctl kern.kwq.bulk.queue 2>/dev/null | awk -F': ' '/\.cost_ns: / && $2 > 0 {print $2}' | sort -n |
		    awk -v t=$(date +%s) '{a[NR] = $1} END {if (NR) {k = int(NR * 0.9); if (k < 1) k = 1;
			printf "%s %d %d %d %d %d\n", t, NR, a[1], a[int((NR + 1) / 2)], a[k], a[NR]}}'
		sleep 2
	done > $1
}
i=0
for b in "0 0"; do
	set -- $b; i=$((i + 1)); tag=d$1_u$2_$i
	doas -n sysctl -q kern.kwq.bulk.shard.directed=$1 kern.kwq.bulk.shard.directed_us=$2 >/dev/null
	log "block $i: set shard.directed=$1 shard.directed_us=$2"
	rm -f $D/sampler.stop; sampler $O/cost_$tag.txt & sp=$!
	c0=$(sums)
	doas -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/ssdab3_$tag.tsv" SETTINGS_LOG="$L" \
	    REPS=3 PROBE_S=3 SSD_N=16 SSD_MDSIZE=256 SSD_LOADS=randread TESTS=ssd \
	    sh "$B" kwq kwqshard > "$O/ssdab3_$tag.log" 2>&1
	log "geli_bench ssd exit $?"
	c1=$(sums); touch $D/sampler.stop; wait $sp
	echo $c0 $c1 | awk -v t=$tag '{printf "%s wakes %d steals %d directed %d chained %d\n", t, $5-$1, $6-$2, $7-$3, $8-$4}' >> $O/ssdab3_counters.txt
done
rm -f $D/sampler.stop
doas -n sysctl -q kern.kwq.bulk.place=0 kern.kwq.bulk.shard.directed=0 kern.kwq.bulk.shard.directed_us=0 >/dev/null
doas -n kldunload kwq
log "restored the knobs, unloaded kwq"
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(doas -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/ssdab3.done
