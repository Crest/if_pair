#!/bin/sh
# prio_lab.sh - does it matter that BULK outranks md(4)?  geli_bench scale
# at 32 and 128 providers, 3 repetitions: stock GELI; kwq-geli with BULK at
# its default priority (PI_SOFT, 2, above md's PRIBIO, 43) and at
# kern.kwq.bulk.priority=55 (below md, as stock GELI's workers at PUSER
# are), each with the share limit on and off.  sh prio_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out/prio; mkdir -p $O; rm -f $D/prio.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) prio: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
bench() {	# TAG STAGE
	sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_$1.tsv" SETTINGS_LOG="$L" REPS=3 PROBE_S=3 IDLE_S=3 \
	    SCALE_NS="32 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $2 > "$O/scale_$1.log" 2>&1
	log "geli_bench $1 exit $?"
}
for prio in default 55; do
	[ $prio = default ] || { sudo -n kenv kern.kwq.bulk.priority=$prio >/dev/null; log "set kenv kern.kwq.bulk.priority=$prio"; }
	sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
	log "loaded kwq.ko: bulk priority $(sysctl -n kern.kwq.bulk.priority) quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_period_us=$(sysctl -n kern.kwq.bulk.cap_period_us)"
	c0=$(sysctl -n kern.kwq.bulk.cap_permille)
	bench p${prio}_cap kwqshard
	sudo -n sysctl -q kern.kwq.bulk.cap_permille=1000 >/dev/null; log "set kern.kwq.bulk.cap_permille=1000 (was $c0)"
	bench p${prio}_nocap kwqshard
	sudo -n sysctl -q kern.kwq.bulk.cap_permille=$c0 >/dev/null; log "restored kern.kwq.bulk.cap_permille=$c0"
	[ $prio = default ] && bench eli eli
	sudo -n kldunload kwq; log "unloaded kwq"
	[ $prio = default ] || { sudo -n kenv -u kern.kwq.bulk.priority; log "unset kenv kern.kwq.bulk.priority"; }
done
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop'), md $(sudo -n mdconfig -l | wc -w), kenv $(kenv -q kern.kwq.bulk.priority 2>/dev/null || echo unset), uptime $(uptime)"
touch $D/prio.done
