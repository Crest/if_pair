#!/bin/sh
# k4_lab.sh - P6g.3 round 2 on lab: kwq_crypto, 1 and 4 stealable queues fed
# on CPU 1, 16 x 64 KiB in flight, under today's code, P8 and its knobs,
# gossip with a lower H (K4).  Run on lab as crest, under sh:  sh k4_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out; mkdir -p $O; rm -f $D/k4.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) k4: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
sudo -n kldload $S/kwq.ko && sudo -n kldload $S/kwq_crypto.ko || { log "cannot load"; exit 1; }
log "loaded kwq.ko kwq_crypto.ko ($S)"
K=$O/k4_lab.tsv; : > $K
set_cfg() {	# place knobs...
	sudo -n sysctl -q kern.kwq.bulk.place=$1 kern.kwq.bulk.shard.spill_idle=0 kern.kwq.bulk.shard.helper_donor=0 \
	    kern.kwq.bulk.shard.donor_us=500 kern.kwq.bulk.gossip.h_us=2000 >/dev/null
	shift; [ $# -gt 0 ] && sudo -n sysctl -q "$@" >/dev/null
}
for rep in 1 2 3; do
	for cfg in "today:0" "p8:2" "p8_spill:2 kern.kwq.bulk.shard.spill_idle=1" "p8_helper:2 kern.kwq.bulk.shard.helper_donor=1" \
	    "p8_d50:2 kern.kwq.bulk.shard.donor_us=50" "p8_d50_spill:2 kern.kwq.bulk.shard.donor_us=50 kern.kwq.bulk.shard.spill_idle=1" \
	    "gossip:1" "gossip_h200:1 kern.kwq.bulk.gossip.h_us=200"; do
		name=${cfg%%:*}; set_cfg ${cfg#*:}
		for nq in 1 4; do
			sudo -n sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=$nq kern.kwq_crypto.depth=16 \
			    kern.kwq_crypto.cpu=1 kern.kwq_crypto.stealable=1 kern.kwq_crypto.secs=3 \
			    kern.kwq_crypto.verify=0 kern.kwq_crypto.bytes=65536 >/dev/null
			sudo -n sysctl -q kern.kwq_crypto.run=1 >/dev/null
			while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
			printf '%s\t%s\t%s\t%s\n' $name $nq $rep "$(dmesg | grep 'kwq_crypto: kwq nq' | tail -1)" >> $K
		done
	done
done
set_cfg 0
sudo -n kldunload kwq_crypto; sudo -n kldunload kwq
log "end, $(wc -l < $K) rows, modules $(kldstat | grep -c kwq), uptime $(uptime)"
touch $D/k4.done
