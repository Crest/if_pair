#!/bin/sh
# Expected use: IPv6 between host and jail over a pair.  Retries
# absorb duplicate address detection on the fresh addresses.
. "$(dirname "$0")/lib.sh"
test_init

A6=2001:db8::1; B6=2001:db8::2
J1=$(jname 1)
create_pair
mkjail "$J1"
must "move ${PAIRB} into ${J1}" ifconfig "$PAIRB" vnet "$J1"
must "address the host side" ifconfig "$PAIRA" inet6 "${A6}/128" "$B6"
must "address the jail side" \
    jexec "$J1" ifconfig "$PAIRB" inet6 "${B6}/128" "$A6" up
must_retry 8 "host -> jail ping6" ping -6 -q -o -c 3 -t 2 "$B6"
must_retry 8 "jail -> host ping6" jexec "$J1" ping -6 -q -o -c 3 -t 2 "$A6"
pass
