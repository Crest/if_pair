#!/bin/sh
# blocks.sh - the time to encrypt or decrypt one GELI block: kwq_crypto
# (AES-XTS-256 through crypto(9), one operation per sector, as GELI) on one
# BULK queue on CPU 1, 16 requests in flight, 3 s per run, the machine
# otherwise quiet.  Requests (bytes:sector) 4096:4096, 32768:32768,
# 1048576:4096, 1048576:32768, 1048576:1048576; encrypt and decrypt; with
# the key on every operation as GELI passes it (rekey 1) and without
# (rekey 0, the session's key); 3 repetitions.  One row per run: rep bytes
# sector rekey decrypt items window_ms mbps_x100 driver cpus errors state.
#   sh blocks.sh lab|a07|guest STAGEDIR OUT
M=${1:?lab|a07|guest}; S=${2:?STAGEDIR}; O=${3:?OUT}
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) blocks: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) blocks: $*" | doas -n tee -a "$L"; } ;;
guest) SU=""; L=/root/ifpair-scratch/settings_guest.log
	log() { echo "$(date -u +%FT%TZ) blocks: $*" | tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
$SU kldload $S/kwq.ko && $SU kldload $S/kwq_crypto.ko || { log "cannot load kwq, kwq_crypto"; exit 1; }
log "loaded kwq.ko, kwq_crypto.ko"
: > "$O"
for rep in 1 2 3; do
	for c in 4096:4096 32768:32768 1048576:4096 1048576:32768 1048576:1048576; do
		b=${c%:*}; s=${c#*:}
		for rk in 1 0; do
			for dec in 0 1; do
				$SU sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=1 kern.kwq_crypto.depth=16 \
				    kern.kwq_crypto.cpu=1 kern.kwq_crypto.stealable=0 kern.kwq_crypto.secs=3 \
				    kern.kwq_crypto.verify=0 kern.kwq_crypto.bytes=$b kern.kwq_crypto.sector=$s kern.kwq_crypto.chunk=$b \
				    kern.kwq_crypto.rekey=$rk kern.kwq_crypto.decrypt=$dec >/dev/null || { log "knobs refused: $c"; break 4; }
				$SU sysctl -q kern.kwq_crypto.run=1 >/dev/null
				while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.2; done
				st=$(sysctl -n kern.kwq_crypto.result_state)
				[ "$st" = done ] || log "$c rekey $rk decrypt $dec: $st: $(sysctl -n kern.kwq_crypto.result_msg)"
				echo "$rep $b $s $rk $dec $(sysctl -n kern.kwq_crypto.result_items kern.kwq_crypto.result_window_ms kern.kwq_crypto.result_mbps_x100 | tr '\n' ' ')$(sysctl -n kern.kwq_crypto.result_driver | tr ' ' '_') $(sysctl -n kern.kwq_crypto.result_cpus kern.kwq_crypto.result_errors | tr '\n' ' ')$st" >> "$O"
			done
		done
	done
done
$SU kldunload kwq_crypto; $SU kldunload kwq
log "end, $(wc -l < "$O" | tr -d ' ') runs, modules $(kldstat | grep -ci kwq), uptime $(uptime)"
