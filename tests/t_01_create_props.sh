#!/bin/sh
# Expected use: a created pair has both sides, point-to-point flags,
# the 16384 default MTU, and both sides in the "pair" group.
. "$(dirname "$0")/lib.sh"
test_init

create_pair
must "${PAIRA} exists" ifconfig "$PAIRA"
must "${PAIRB} exists" ifconfig "$PAIRB"
for i in "$PAIRA" "$PAIRB"; do
	must "$i is POINTOPOINT" \
	    sh -c "ifconfig $i | head -1 | grep -q POINTOPOINT"
	mustfail "$i is not BROADCAST" \
	    sh -c "ifconfig $i | head -1 | grep -q BROADCAST"
	must "$i default mtu is 16384" \
	    sh -c "ifconfig $i | head -1 | grep -qw 16384"
	must "$i is in group pair" \
	    sh -c "ifconfig -g pair | grep -qx $i"
done
pass
