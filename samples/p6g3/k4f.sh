#!/bin/sh
# k4f.sh - P6g.3 K4 round 6 (lab or a07): kwq_crypto, 1 and 4 stealable
# queues fed on CPU 1, 16 x 64 KiB in flight, today against P8 with
# shard.directed 1 (mailbox) and 2 (the chain), the chain with spill_idle=1.
# Each row adds the run's sums over CPUs of steal_wakes, steals, directed,
# chained.  Run as crest under sh:  sh k4f.sh lab|a07 DIR
M=${1:?lab|a07}; D=${2:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/k4f.done
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) k4f: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) k4f: $*" | doas -n tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
$SU kldload $S/kwq.ko && $SU kldload $S/kwq_crypto.ko || { log "cannot load"; exit 1; }
log "loaded kwq.ko kwq_crypto.ko ($S)"
K=$O/k4f_$M.tsv; : > $K
set_cfg() {	# place knobs...
	$SU sysctl -q kern.kwq.bulk.place=$1 kern.kwq.bulk.shard.spill_idle=0 kern.kwq.bulk.shard.helper_donor=0 kern.kwq.bulk.shard.directed=0 \
	    kern.kwq.bulk.shard.donor_us=500 kern.kwq.bulk.shard.directed_us=0 >/dev/null
	shift; [ $# -gt 0 ] && $SU sysctl -q "$@" >/dev/null
}
sums() {	# steal_wakes steals directed chained, over the BULK workers
	sysctl kern.kwq.bulk.cpu | awk -F': ' '
	    /\.steal_wakes:/ {w += $2} /\.steals:/ {s += $2} /\.directed:/ {d += $2} /\.chained:/ {c += $2}
	    END {printf "%d %d %d %d\n", w, s, d, c}'
}
for rep in 1 2 3; do
	for cfg in "today:0" "p8_chain:2 kern.kwq.bulk.shard.directed=2" \
	    "p8_chain_us10:2 kern.kwq.bulk.shard.directed=2 kern.kwq.bulk.shard.directed_us=10"; do
		name=${cfg%%:*}; set_cfg ${cfg#*:}
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
set_cfg 0
$SU kldunload kwq_crypto; $SU kldunload kwq
log "end, $(wc -l < $K) rows, modules $(kldstat | grep -c kwq), uptime $(uptime)"
touch $D/k4f.done
