#!/bin/sh
# probes_a07.sh (a07, crest): probes.d over probes_load.sh with the deployed
# stage; dtrace stays loaded (it is a07's own), modules unloaded at the end.
D=${1:?DIR}; S=$D/stage; B=$D/p6g4; L=$HOME/if_pair-scratch/settings_a07.log
log() { echo "$(date -u +%FT%TZ) probes: $*" | doas -n tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
doas -n kldload $S/kwq.ko && doas -n kldload $S/kwq_test.ko && doas -n kldload $S/kwq_crypto.ko || { log "cannot load"; exit 1; }
log "loaded kwq.ko kwq_test.ko kwq_crypto.ko: place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us)"
doas -n dtrace -L $D/src/kwq/kwq -q -s $B/probes.d -c "sh $B/probes_load.sh" > $B/probes_a07.out 2>&1
log "dtrace exit $?"
doas -n sysctl -q kern.kwq.bulk.place=2 >/dev/null
doas -n kldunload kwq_crypto; doas -n kldunload kwq_test; doas -n kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli'), uptime $(uptime)"
