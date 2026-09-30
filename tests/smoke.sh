#!/bin/sh
# Smoke test for if_pair(4): create a pair, put each side in a vnet jail,
# ping across it with IPv4 and IPv6.  Must run as root on FreeBSD 15+.

set -eu

JAIL_A=pairtest_a
JAIL_B=pairtest_b
BASE=""
# PAIR=pairtq PAIR_MOD=if_pair_tq selects the taskqueue baseline (lib.sh).
: "${PAIR:=pair}"
: "${PAIR_MOD:=if_pair}"

cleanup() {
	jail -r ${JAIL_A} 2>/dev/null || true
	jail -r ${JAIL_B} 2>/dev/null || true
	[ -n "${BASE}" ] && ifconfig "${BASE}a" destroy 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root" >&2
	exit 1
fi

if ! kldstat -q -m ${PAIR_MOD}; then
	d=$(dirname "$0")
	if [ "${PAIR_MOD}" = if_pair ] && ! kldstat -q -m kwq; then
		for ko in "${KWQ_KO:-}" "$d/../kwq/kwq/kwq.ko" kwq; do
			[ -n "$ko" ] && kldload "$ko" 2>/dev/null && break
		done
	fi
	for ko in "$d/../${PAIR_MOD}.ko" "$d/../extras/${PAIR_MOD}/${PAIR_MOD}.ko" "$d/${PAIR_MOD}.ko"; do
		[ -f "$ko" ] && { kldload "$ko"; break; }
	done
fi

# ifconfig prints the name of the created 'a' side, e.g. pair0a.
side_a=$(ifconfig ${PAIR} create)
BASE=${side_a%a}
side_b="${BASE}b"
echo "created ${side_a} / ${side_b}"

jail -c name=${JAIL_A} vnet persist
jail -c name=${JAIL_B} vnet persist

ifconfig "${side_a}" vnet ${JAIL_A}
ifconfig "${side_b}" vnet ${JAIL_B}

jexec ${JAIL_A} ifconfig "${side_a}" inet 192.0.2.1/32 192.0.2.2 up
jexec ${JAIL_B} ifconfig "${side_b}" inet 192.0.2.2/32 192.0.2.1 up
jexec ${JAIL_A} ifconfig "${side_a}" inet6 2001:db8::1/128 2001:db8::2
jexec ${JAIL_B} ifconfig "${side_b}" inet6 2001:db8::2/128 2001:db8::1

fail=0
echo "--- IPv4 ping"
jexec ${JAIL_A} ping -c 3 -t 5 192.0.2.2 || fail=1
echo "--- IPv6 ping"
jexec ${JAIL_A} ping -6 -c 3 -t 5 2001:db8::2 || fail=1

# Destroying either side removes both halves; exercise the 'b' path.
echo "--- destroy via b side must remove both halves"
if ! jexec ${JAIL_B} ifconfig "${side_b}" destroy; then
	echo "ERROR: destroying ${side_b} failed" >&2
	fail=1
elif jexec ${JAIL_A} ifconfig "${side_a}" >/dev/null 2>&1; then
	echo "ERROR: ${side_a} still exists after destroying ${side_b}" >&2
	fail=1
else
	BASE=""	# pair is gone; nothing for the cleanup trap to destroy
fi

if [ ${fail} -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL" >&2
fi
exit ${fail}
