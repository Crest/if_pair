#!/bin/sh
# p6g0_lab.sh - SPEC-p6g.md P6g.0 on lab, as root under sh:
#
#   sh p6g0_lab.sh DIR     # DIR/A, DIR/B from bulk_deploy.sh: A today's
#                          # order, B built with KWQ_SIBLING_LAST=1
#
# 1. Line costs (userland, pinned: a calibration, not a result): pp between
#    SMT siblings and between cores.
# 2. The kernel's SMT yield (build A): kwq_crypto, not stealable, one queue
#    on CPU 2, two on the siblings 2 and 3, two on the cores 2 and 4;
#    4 KiB and 1 MiB requests.
# 3. K0, A against B, alternating per repetition (modules reloaded):
#    kwq_crypto KWQ_CPU_ANY with 2, 4, 8 stealable queues (1 MiB); four
#    stealable queues fed on CPU 2 (the steal spread); 16 kwq-geli
#    providers on md(4), 4 KiB reads and 1 MiB writes (iolat, 8 s).
# Each run records MB/s, the CPUs that did more than 5 % of the BULK busy
# time, and the cores on which both threads did (lab's pairs: 2k, 2k+1).
# Every module and md device is logged to the settings log with its undo;
# no system setting is changed.
D=${1:?DIR}
L=/home/crest/if_pair-scratch/settings_lab.log; O=$D/out
mkdir -p "$O"; rm -f "$D/p6g0.done" "$O/p6g0.txt"
note() { echo "$(date -u +%FT%TZ) p6g0: $*" >> "$L"; }
note "start, uptime $(uptime)"
for m in kwq kwq_crypto g_kwqeli dtraceall; do
	kldstat -q -m $m && { note "$m loaded before the run: abort"; exit 1; }
done
[ -z "$(mdconfig -l)" ] || { note "md devices exist before the run: abort"; exit 1; }
cc -O2 -o "$O/iolat" "$D/A/src/kwq-geli/tests/iolat.c" -lpthread -lm || exit 1
cc -O2 -o "$O/pp" "$D/pp.c" -lpthread || exit 1
MDS=""; CUR=""; DEVS=""
unload() {
	for m in $DEVS; do env GEOM_LIBRARY_PATH="$D/$CUR/stage" geom kwqeli detach -f $m 2>/dev/null; done
	DEVS=""
	kldstat -q -m kwq_crypto && kldunload kwq_crypto && note "unloaded kwq_crypto ($CUR)"
	kldstat -q -m g_kwqeli && kldunload geom_kwqeli && note "unloaded geom_kwqeli ($CUR)"
	kldstat -q -m kwq && kldunload kwq && note "unloaded kwq ($CUR)"
	CUR=""
}
cleanup() {
	trap - EXIT INT TERM HUP
	unload
	for m in $MDS; do mdconfig -d -u $m && note "destroyed $m"; done
	note "end, modules $(kldstat | grep -cE 'kwq|dtrace'), md $(mdconfig -l | wc -w), uptime $(uptime)"
	touch "$D/p6g0.done"
}
trap cleanup EXIT INT TERM HUP
load() {	# A|B
	unload
	kldload "$D/$1/stage/kwq.ko" && note "loaded kwq.ko ($1)" || exit 1
	kldload "$D/$1/stage/kwq_crypto.ko" && note "loaded kwq_crypto.ko ($1)" || exit 1
	CUR=$1
}
loadgeli() {
	kldload "$D/$CUR/stage/geom_kwqeli.ko" && note "loaded geom_kwqeli.ko ($CUR)" || exit 1
	for m in $MDS; do
		env GEOM_LIBRARY_PATH="$D/$CUR/stage" geom kwqeli onetime -e AES-XTS -l 256 -s 4096 $m || exit 1
		DEVS="$DEVS $m.kweli"
	done
}
snap() { sysctl kern.kwq.bulk.cpu | awk -F': ' '/\.busy_ns:/ { split($1, a, "."); print a[5], $2 }' | sort > "$1"; }
spread() {	# BEFORE AFTER -> "cpus=N both=M busy_ms=T"
	join "$1" "$2" | awk '{ d[$1] = $3 - $2; t += $3 - $2 }
	    END { for (c in d) if (d[c] > 0.05 * t) { u[c] = 1; n++ }
		for (c in u) if (c % 2 == 0 && ((c + 1) in u)) b++
		printf("cpus=%d both=%d busy_ms=%d", n, b, t / 1e6) }'
}
kc() {	# TAG knob=value ...: one kwq_crypto run of 5 s
	tag=$1; shift
	sysctl -q kern.kwq_crypto.mode=kwq kern.kwq_crypto.nq=1 kern.kwq_crypto.cpu=-1 \
	    kern.kwq_crypto.cpustep=0 kern.kwq_crypto.stealable=0 kern.kwq_crypto.bytes=1048576 \
	    kern.kwq_crypto.sector=4096 kern.kwq_crypto.depth=4 kern.kwq_crypto.secs=5 "$@" > /dev/null
	snap "$O/s0"
	sysctl -q kern.kwq_crypto.run=1 > /dev/null
	sleep 1
	while [ "$(sysctl -n kern.kwq_crypto.result_state)" = running ]; do sleep 0.5; done
	snap "$O/s1"
	st=$(sysctl -n kern.kwq_crypto.result_state)
	[ "$st" = done ] || note "kwq_crypto $tag: $st $(sysctl -n kern.kwq_crypto.result_msg)"
	echo "build=$CUR rep=$rep case=$tag state=$st mbps=$(sysctl -n kern.kwq_crypto.result_mbps_x100 | awk '{ printf("%.1f", $1 / 100) }') p99_us=$(sysctl -n kern.kwq_crypto.result_p99_us) $(spread "$O/s0" "$O/s1")" >> "$O/p6g0.txt"
}
io() {	# TAG IOLAT-OPTIONS
	snap "$O/s0"
	"$O/iolat" $2 -t 8 $(for m in $DEVS; do echo /dev/$m; done) > "$O/io" 2>&1 || { note "iolat failed: $(cat "$O/io")"; exit 1; }
	snap "$O/s1"
	echo "build=$CUR rep=$rep case=$1 $(cat "$O/io") $(spread "$O/s0" "$O/s1")" >> "$O/p6g0.txt"
}

# 1. Line costs.
for p in "2 3" "2 4" "2 10" "2 15" "0 1" "0 8"; do
	echo "pp $("$O/pp" $p 2000000)" >> "$O/p6g0.txt"
done

# 2. SMT yield, build A.
load A
for rep in 1 2 3; do
	for b in 4096 1048576; do
		kc yield_one_$b kern.kwq_crypto.cpu=2 kern.kwq_crypto.bytes=$b kern.kwq_crypto.depth=8
		kc yield_sib_$b kern.kwq_crypto.nq=2 kern.kwq_crypto.cpu=2 kern.kwq_crypto.cpustep=1 kern.kwq_crypto.bytes=$b kern.kwq_crypto.depth=8
		kc yield_core_$b kern.kwq_crypto.nq=2 kern.kwq_crypto.cpu=2 kern.kwq_crypto.cpustep=2 kern.kwq_crypto.bytes=$b kern.kwq_crypto.depth=8
	done
done

# 3. K0: A against B.
for i in $(jot 16); do
	m=$(mdconfig -a -t swap -s 32m) || exit 1
	MDS="$MDS $m"; note "created $m (swap 32m)"
	dd if=/dev/zero of=/dev/$m bs=1m count=32 2> /dev/null || exit 1
done
for rep in 1 2 3; do
	case $rep in 2) order="B A" ;; *) order="A B" ;; esac
	for v in $order; do
		load $v
		for n in 2 4 8; do
			kc any_$n kern.kwq_crypto.nq=$n kern.kwq_crypto.stealable=1
		done
		kc one4 kern.kwq_crypto.nq=4 kern.kwq_crypto.cpu=2 kern.kwq_crypto.stealable=1 kern.kwq_crypto.depth=8
		loadgeli
		io geli16_small "-r -b 4096 -q 1"
		io geli16_bulk "-w -b 1048576 -q 1"
	done
done
sysctl -n kern.kwq.topo > "$O/topo_B.txt"
load A; sysctl -n kern.kwq.topo > "$O/topo_A.txt"
