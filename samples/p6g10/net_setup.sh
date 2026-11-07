#!/bin/sh
# net_setup.sh up|down NP: NP kpair pairs, each between two vnet jails
# ifp_kp_s<i> and ifp_kp_c<i>, addresses 10.99.<i>.1 and .2, MTU 16384, the
# server jail's ICMP limit lifted.  As root, if_kpair loaded.  down waits for
# the jails to be gone, 2 s more and the interfaces back, and fails if they do
# not come: unloading a cloner during a vnet's teardown can deadlock the base
# kernel (bugs/vnet-kldunload-deadlock), so the caller unloads only after a 0.
A=${1:?up|down}; NP=${2:?NP}
J="jail -f /dev/null"
case $A in
up)
	i=1
	while [ $i -le "$NP" ]; do
		a=$(ifconfig kpair create) || exit 1
		case $a in kpair*a) ;; *) echo "net_setup: created $a" >&2; exit 1 ;; esac
		b=${a%a}b; s=ifp_kp_s$i; c=ifp_kp_c$i
		sysctl -n "kern.kwq.net.queue.$a.limit" >/dev/null || { echo "net_setup: $a is not on kwq" >&2; exit 1; }
		$J -c name=$s path=/ vnet=new persist >/dev/null && $J -c name=$c path=/ vnet=new persist >/dev/null || exit 1
		ifconfig "$a" vnet $s && ifconfig "$b" vnet $c || exit 1
		jexec $s ifconfig "$a" mtu 16384 && jexec $s ifconfig "$a" inet 10.99.$i.1/32 10.99.$i.2 up || exit 1
		jexec $c ifconfig "$b" mtu 16384 && jexec $c ifconfig "$b" inet 10.99.$i.2/32 10.99.$i.1 up || exit 1
		jexec $s sysctl -q net.inet.icmp.icmplim=1000000 >/dev/null || exit 1
		n=0
		until jexec $c ping -q -o -c 2 -t 2 10.99.$i.1 >/dev/null 2>&1; do
			n=$((n + 1)); [ $n -lt 5 ] || { echo "net_setup: pair $i does not answer" >&2; exit 1; }
			sleep 1
		done
		i=$((i + 1))
	done ;;
down)
	i=1
	while [ $i -le "$NP" ]; do
		$J -r ifp_kp_s$i >/dev/null 2>&1; $J -r ifp_kp_c$i >/dev/null 2>&1
		i=$((i + 1))
	done
	# A dying jail stays until its TCP connections' TIME_WAIT ends, about a
	# minute, its interface with it; the cloner must outlive all of that.
	n=0
	while jls -d name | grep -q '^ifp_kp_'; do
		n=$((n + 1)); [ $n -lt 300 ] || { echo "net_setup: jails still dying: $(jls -d name | grep -c '^ifp_kp_')" >&2; exit 1; }
		sleep 1
	done
	sleep 2
	# the interfaces came home with the vnets' end; a pair goes with its a side
	made=$(sysctl -N kern.kwq.net.queue 2>/dev/null | grep -c '^kern\.kwq\.net\.queue\.kpair[0-9]*a\.reset$')
	n=0
	while [ "$(ifconfig -l | tr ' ' '\n' | grep -c '^kpair[0-9]*a$')" -lt "$made" ]; do
		n=$((n + 1)); [ $n -lt 60 ] || { echo "net_setup: $made pairs made, $(ifconfig -l) here" >&2; exit 1; }
		sleep 1
	done
	for a in $(ifconfig -l | tr ' ' '\n' | grep '^kpair[0-9]*a$'); do ifconfig "$a" destroy; done
	sleep 2
	[ -z "$(ifconfig -l | tr ' ' '\n' | grep '^kpair')" ] || { echo "net_setup: kpair interfaces left: $(ifconfig -l)" >&2; exit 1; } ;;
esac
