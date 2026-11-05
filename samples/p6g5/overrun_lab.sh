#!/bin/sh
# overrun_lab.sh - overrun.d around geli_bench scale at 16 and 128
# providers, the kwqshard stage (P8 at the defaults), REPS=1; dtraceall
# loaded and unloaded again through the settings log.  sh overrun_lab.sh DIR
D=${1:?DIR}; S=$D/stage; O=$D/out/overrun; mkdir -p $O; rm -f $D/overrun.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) overrun: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko: place=$(sysctl -n kern.kwq.bulk.place) quantum_us=$(sysctl -n kern.kwq.bulk.quantum_us) cap_permille=$(sysctl -n kern.kwq.bulk.cap_permille) penalty_rounds=$(sysctl -n kern.kwq.bulk.penalty_rounds) budget_check_every=$(sysctl -n kern.kwq.bulk.budget_check_every)"
dl=0; kldstat -q -m dtraceall || { sudo -n kldload dtraceall && dl=1 && log "loaded dtraceall"; }
cat > $O/run.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_overrun.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="16 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" kwqshard > "$O/scale_overrun.log" 2>&1
EOS
sudo -n dtrace -L $D/src/kwq/kwq -s $D/overrun.d -c "sh $O/run.sh" > $O/overrun_d.out 2>&1
log "overrun.d over kwqshard: exit $?"
[ $dl = 1 ] && sudo -n kldunload dtraceall && log "unloaded dtraceall"
sudo -n kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|dtrace'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/overrun.done
