#!/bin/sh
# Smoke test for if_pair(4): create a pair, put each side in a vnet jail,
# ping across it with IPv4 and IPv6.  Must run as root on FreeBSD 15+.

set -eu

JAIL_A=pairtest_a
JAIL_B=pairtest_b
PAIR=""

cleanup() {
	jail -r ${JAIL_A} 2>/dev/null || true
	jail -r ${JAIL_B} 2>/dev/null || true
	[ -n "${PAIR}" ] && ifconfig "${PAIR}a" destroy 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root" >&2
	exit 1
fi

if ! kldstat -q -m if_pair; then
	kldload "$(dirname "$0")/../if_pair.ko"
fi

# ifconfig prints the name of the created 'a' side, e.g. pair0a.
side_a=$(ifconfig pair create)
PAIR=${side_a%a}
side_b="${PAIR}b"
echo "created ${side_a} / ${side_b}"

jail -c name=${JAIL_A} vnet persist
jail -c name=${JAIL_B} vnet persist

ifconfig "${side_a}" vnet ${JAIL_A}
ifconfig "${side_b}" vnet ${JAIL_B}

jexec ${JAIL_A} ifconfig "${side_a}" inet 192.0.2.1/31 up
jexec ${JAIL_B} ifconfig "${side_b}" inet 192.0.2.0/31 up
jexec ${JAIL_A} ifconfig "${side_a}" inet6 2001:db8::1/127
jexec ${JAIL_B} ifconfig "${side_b}" inet6 2001:db8::/127

fail=0
echo "--- IPv4 ping"
jexec ${JAIL_A} ping -c 3 -t 5 192.0.2.0 || fail=1
echo "--- IPv6 ping"
jexec ${JAIL_A} ping -6 -c 3 -t 5 2001:db8:: || fail=1

# Destroying the 'b' side directly must be refused.
echo "--- destroy of b side must fail"
if jexec ${JAIL_B} ifconfig "${side_b}" destroy 2>/dev/null; then
	echo "ERROR: destroying ${side_b} unexpectedly succeeded" >&2
	fail=1
fi

if [ ${fail} -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL" >&2
fi
exit ${fail}
