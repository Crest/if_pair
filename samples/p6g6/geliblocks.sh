#!/bin/sh
# geliblocks.sh - CPU time per GELI block inside GELI: stock GELI (one
# worker per provider, kern.geom.eli.threads=1, pinned to CPU 1 with
# cpuset) and kwq-geli (every bio on CPU 1's BULK worker, not stealable),
# onetime AES-XTS-256 providers of 4 KiB and 32 KiB sectors on a 1 GiB
# swap-backed md(4) written whole once.  iolat from the other CPUs, 16
# requests in flight, sequential, 4 s: writes (encryption) and reads
# (decryption) of one block and of 1 MiB.  Per block: the worker thread's
# CPU time over the run (ps, 10 ms resolution) / the blocks it processed;
# that includes GELI's own work per bio (queueing, wakeups, a write's
# buffer and copy).  3 repetitions, stages interleaved.  One row per run:
# rep stage sector bytes op ops cpu_cs us_per_block mbps.
#   [REPS=3] sh geliblocks.sh lab|a07|guest DIR OUT   (DIR with stage/ and src/)
M=${1:?lab|a07|guest}; D=${2:?DIR}; O=${3:?OUT}; S=$D/stage; REPS=${REPS:-3}
case $M in
lab) SU="sudo -n"; L=$HOME/if_pair-scratch/settings_lab.log
	log() { echo "$(date -u +%FT%TZ) geliblocks: $*" | tee -a "$L"; } ;;
a07) SU="doas -n"; L=$HOME/if_pair-scratch/settings_a07.log
	log() { echo "$(date -u +%FT%TZ) geliblocks: $*" | doas -n tee -a "$L"; } ;;
guest) SU=""; L=/root/ifpair-scratch/settings_guest.log
	log() { echo "$(date -u +%FT%TZ) geliblocks: $*" | tee -a "$L"; } ;;
*) exit 2 ;;
esac
log "start, uptime $(uptime)"
for m in kwq kwq_test kwq_crypto g_kwqeli g_nop; do kldstat -q -m $m && { log "$m loaded: abort"; exit 1; }; done
[ -z "$($SU mdconfig -l)" ] || { log "md devices exist: abort"; exit 1; }
T=$(mktemp -d /tmp/geliblocks.XXXXXX) || exit 1
cc -O2 -o $T/iolat $D/src/kwq-geli/tests/iolat.c -lpthread -lm || exit 1
NCPU=$(sysctl -n hw.ncpu); OTHERS="0,2-$((NCPU - 1))"
GK="env GEOM_LIBRARY_PATH=$S geom kwqeli"
cs() { awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf("%d\n", s * 100 + 0.5) }'; }
tcpu() {	# eli PID | kwq: the thread's CPU time in centiseconds
	if [ "$1" = kwq ]; then
		ps -axH -o time= -o tdname= | awk '$2 == "kwq_bulk/1" { print $1 }' | cs
	else
		ps -p "$1" -o time= | tr -d ' ' | cs
	fi
}
eload=0; kldstat -q -m g_eli || { $SU kldload geom_eli && eload=1 && log "loaded geom_eli"; }
th0=$(sysctl -n kern.geom.eli.threads) || exit 1
$SU kldload $S/kwq.ko && $SU kldload $S/geom_kwqeli.ko || { log "cannot load kwq, geom_kwqeli"; exit 1; }
c0=$(sysctl -n kern.geom.kwqeli.cpu); s0=$(sysctl -n kern.geom.kwqeli.stealable)
$SU sysctl -q kern.geom.eli.threads=1 kern.geom.kwqeli.cpu=1 kern.geom.kwqeli.stealable=0 >/dev/null
log "loaded kwq.ko, geom_kwqeli.ko; set kern.geom.eli.threads=1 (was $th0) kern.geom.kwqeli.cpu=1 (was $c0) kern.geom.kwqeli.stealable=0 (was $s0)"
md=$($SU mdconfig -a -t swap -s 1g) || exit 1
$SU dd if=/dev/zero of=/dev/$md bs=1m 2>/dev/null
log "created $md (1 GiB swap-backed, written)"
: > "$O"
for rep in $(jot $REPS); do
	for st in eli kwq; do
		for sec in 4096 32768; do
			if [ $st = eli ]; then
				$SU geli onetime -e AES-XTS -l 256 -s $sec $md || break 3; dev=$md.eli
				who=$(pgrep -S "^g_eli\[[0-9]+\] $md\$")
				[ "$(echo $who | wc -w)" -eq 1 ] || { log "GELI workers of $md: $who, not one: abort"; break 3; }
				$SU cpuset -l 1 -p "$who" || break 3
			else
				$SU $GK onetime -e AES-XTS -l 256 -s $sec $md || break 3; dev=$md.kweli; who=kwq
			fi
			for b in $sec 1048576; do
				for op in write read; do
					w=; [ $op = write ] && w=-w
					a=$(tcpu "$who")
					$SU cpuset -l $OTHERS $T/iolat $w -b $b -q 16 -t 4 /dev/$dev > $T/io 2>&1
					z=$(tcpu "$who")
					tr ' ' '\n' < $T/io | awk -F= -v r=$rep -v st=$st -v sec=$sec -v b=$b -v op=$op -v a=$a -v z=$z '
					    $1 == "ops" { n = $2 } $1 == "mbps" { m = $2 }
					    END { blk = n * b / sec; printf("%d %s %d %d %s %d %d %.3f %.1f\n", r, st, sec, b, op, n, z - a, blk > 0 ? (z - a) * 10000 / blk : 0, m) }' >> "$O"
				done
			done
			if [ $st = eli ]; then $SU geli detach $dev; else $SU $GK detach $dev; fi
			while pgrep -S "^g_eli\[[0-9]+\] $md\$" >/dev/null; do sleep 0.2; done
		done
	done
done
$SU mdconfig -d -u ${md#md}
$SU sysctl -q kern.geom.eli.threads=$th0 kern.geom.kwqeli.cpu=$c0 kern.geom.kwqeli.stealable=$s0 >/dev/null
$SU kldunload geom_kwqeli; $SU kldunload kwq
[ $eload = 1 ] && $SU kldunload geom_eli && log "unloaded geom_eli"
rm -rf $T
log "restored kern.geom.eli.threads=$th0 kern.geom.kwqeli.cpu=$c0 kern.geom.kwqeli.stealable=$s0; destroyed $md; unloaded geom_kwqeli, kwq"
log "end, $(wc -l < "$O" | tr -d ' ') runs, modules $(kldstat | grep -cE 'kwq|kwqeli'), md $($SU mdconfig -l | wc -w), uptime $(uptime)"
