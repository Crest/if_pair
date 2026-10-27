#!/bin/sh
# Same hitting shape as reproducer-epair.sh, for if_pair, to test the
# MOD_UNLOAD cloner-detach fix.  kwq stays loaded (if_pair depends on it).
# Each round loads if_pair, builds two vnet jails each holding one end of
# a pair, and unloads if_pair while the first jail's vnet is dying.  No
# in-jail address is configured: the deadlock is in the cloner detach per
# vnet, independent of addresses.  MODDIR holds kwq.ko and if_pair.ko.
ROUNDS=${ROUNDS:-500}
MODDIR=${MODDIR:-/root/ifpairfix/fix}
kldload -n "$MODDIR/kwq.ko" || exit 1
n=0
while [ $n -lt $ROUNDS ]; do
	n=$((n + 1))
	echo "round $n start"
	kldload -n "$MODDIR/if_pair.ko" || exit 1
	pa=$(ifconfig pair create) || exit 1
	pb=$(ifconfig pair create) || exit 1
	jail -c name=lor1 vnet persist >/dev/null || exit 1
	jail -c name=lor2 vnet persist >/dev/null || exit 1
	ifconfig "$pa" vnet lor1 || exit 1
	ifconfig "$pb" vnet lor2 || exit 1
	# first jail runs a process so its vnet dies asynchronously; second
	# jail is bare so its vnet dies in the jail -r syscall
	jexec lor1 sleep 1000 &
	sleep 0.2
	jail -r lor1
	kldunload if_pair &
	kpid=$!
	jail -r lor2
	wait $kpid 2>/dev/null
	kill %1 2>/dev/null; wait 2>/dev/null
	[ $((n % 50)) -eq 0 ] && echo "round $n ok"
done
echo "no deadlock in $ROUNDS rounds"
