#!/bin/sh
# Corner cases: the MTU limits (72..65535) are enforced and the two
# sides have independent MTUs.
. "$(dirname "$0")/lib.sh"
test_init

create_pair
must "set mtu 1500 on a" ifconfig "$PAIRA" mtu 1500
must "b keeps its own mtu 16384" \
    sh -c "ifconfig $PAIRB | head -1 | grep -qw 16384"
must "set minimum mtu 72" ifconfig "$PAIRA" mtu 72
must "set maximum mtu 65535" ifconfig "$PAIRA" mtu 65535
mustfail "mtu 71 is refused" ifconfig "$PAIRA" mtu 71
mustfail "mtu 65536 is refused" ifconfig "$PAIRA" mtu 65536
pass
