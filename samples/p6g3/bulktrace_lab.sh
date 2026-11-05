#!/bin/sh
# bulktrace_lab.sh - why kwq-geli's 1 MiB writes fall behind stock GELI
# above 16 providers on lab (BULK.md S9).  sh bulktrace_lab.sh DIR
# A. bulk.d around geli_bench scale at 16 and 128 providers, REPS=1, for
#    stock GELI and for P8 at the defaults (dtraceall loaded and unloaded
#    again, through the settings log);
# B. geli_bench scale at 128 providers, 3 repetitions: eli, kwq (today),
#    kwqshard (P8), kwqnosteal (not stealable), kwqlocal (each bio on its
#    caller's CPU), then kwqshard with the share limit off
#    (kern.kwq.bulk.cap_permille=1000, restored after).
D=${1:?DIR}; S=$D/stage; O=$D/out/trace; mkdir -p $O; rm -f $D/trace.done
L=$HOME/if_pair-scratch/settings_lab.log
log() { echo "$(date -u +%FT%TZ) trace: $*" | tee -a "$L"; }
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$(sudo -n mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
B="$D/src/kwq-geli/tests/geli_bench.sh"
sudo -n kldload $S/kwq.ko || { log "cannot load kwq"; exit 1; }
log "loaded kwq.ko: place=$(sysctl -n kern.kwq.bulk.place) directed=$(sysctl -n kern.kwq.bulk.shard.directed) directed_us=$(sysctl -n kern.kwq.bulk.shard.directed_us) cap_permille=$(sysctl -n kern.kwq.bulk.cap_permille)"
dl=0; kldstat -q -m dtraceall || { sudo -n kldload dtraceall && dl=1 && log "loaded dtraceall"; }
for st in eli kwqshard; do
	cat > $O/run_$st.sh <<EOS
env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_trace_$st.tsv" SETTINGS_LOG="$L" REPS=1 PROBE_S=3 IDLE_S=3 SCALE_NS="16 128" SCALE_MDSIZE=32 TESTS=scale sh "$B" $st > "$O/scale_trace_$st.log" 2>&1
EOS
	sudo -n dtrace -L $D/src/kwq/kwq -s $D/bulk.d -c "sh $O/run_$st.sh" > $O/bulk_d_$st.out 2>&1
	log "bulk.d over $st: exit $?"
done
[ $dl = 1 ] && sudo -n kldunload dtraceall && log "unloaded dtraceall"
sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_ab128.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS=128 SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" eli kwq kwqshard kwqnosteal kwqlocal > "$O/scale_ab128.log" 2>&1
log "geli_bench scale 128 exit $?"
c0=$(sysctl -n kern.kwq.bulk.cap_permille)
sudo -n sysctl -q kern.kwq.bulk.cap_permille=1000 >/dev/null
log "set kern.kwq.bulk.cap_permille 1000 (was $c0)"
sudo -n env MODDIR="$S" VICTIM="$S/victim" OUT_TSV="$O/scale_ab128_nocap.tsv" SETTINGS_LOG="$L" \
    REPS=3 PROBE_S=3 IDLE_S=3 SCALE_NS=128 SCALE_MDSIZE=32 TESTS=scale \
    sh "$B" kwqshard > "$O/scale_ab128_nocap.log" 2>&1
log "geli_bench scale 128, share limit off, exit $?"
sudo -n sysctl -q kern.kwq.bulk.cap_permille=$c0 >/dev/null
log "restored kern.kwq.bulk.cap_permille $c0"
sudo -n kldunload kwq
log "end, modules $(kldstat | grep -cE 'kwq|kwqeli|geom_nop|dtrace'), md $(sudo -n mdconfig -l | wc -w), uptime $(uptime)"
touch $D/trace.done
