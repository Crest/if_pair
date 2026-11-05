#!/bin/sh
# oneliners_guest.sh (guest, root): each OBSERVABILITY.md stealing one-liner over probes_load.sh
S=/root/ifpair-scratch/stages/p6g2/${1:-pgq}; B=/root/ifpair-scratch/stages/p6g4
kldstat -q -m kwq && { echo "kwq loaded"; exit 1; }
dl=0; kldstat -q -m dtraceall || { kldload dtraceall && dl=1; }
kldload $S/kwq/kwq.ko && kldload $S/kwq_test/kwq_test.ko && kldload $S/kwq_crypto/kwq_crypto.ko || exit 1
n=0
while read -r d; do
	n=$((n + 1))
	echo "== one-liner $n"
	dtrace -L $S -q -n "$d" -c "sh $B/probes_load.sh" 2>&1 | grep -v "^ *$" | grep -v "^steal place\|^anyburst\|^starved place\|^crypto:" | head -8
done <<'D'
kwq:::steal { @[args[0]->kwq_name, arg4] = sum(arg3); }
kwq:::steal /arg4 == KWQ_STEAL_CHAIN/ { @[arg1, arg2] = count(); }
kwq:::steal-wake /arg1 == -1/ { @[arg0, arg2] = count(); }
kwq:::steal-miss { @[arg1, arg3] = count(); }
kwq:::donor /arg1/ { t[arg0] = timestamp; } kwq:::donor /!arg1 && t[arg0]/ { @ = quantize(timestamp - t[arg0]); t[arg0] = 0; } kwq:::donor /arg2/ { @s[arg0] = count(); }
kwq:::place { @[arg3, arg4] = count(); @d[arg1 == arg2 ? "own CPU" : "moved"] = count(); }
D
kldunload kwq_crypto; kldunload kwq_test; kldunload kwq
[ $dl = 1 ] && kldunload dtraceall
echo "modules: $(kldstat | grep -ci kwq)"; dmesg | egrep -i "witness|lock order|panic|assert" | head -3
