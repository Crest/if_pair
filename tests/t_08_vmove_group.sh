#!/bin/sh
# Corner case: "pair" group membership must follow a side into a
# jail (if_clone_restoregroup() keys on the cloner list our explicit
# if_clone_addif() puts the b side on) and return with it when the
# jail dies.
. "$(dirname "$0")/lib.sh"
test_init

J1=$(jname 1)
create_pair
must "b in the host's pair group" \
    sh -c "ifconfig -g pair | grep -qx $PAIRB"
mkjail "$J1"
must "move ${PAIRB} into ${J1}" ifconfig "$PAIRB" vnet "$J1"
must "b in the jail's pair group" \
    sh -c "jexec $J1 ifconfig -g pair | grep -qx $PAIRB"
mustfail "b not in the host group while jailed" \
    sh -c "ifconfig -g pair | grep -qx $PAIRB"

# Jail teardown is asynchronous; the interface returns home when the
# dying vnet is finally destroyed.
must "remove the jail" jail -r "$J1"
must_retry 10 "b returned to the host" ifconfig "$PAIRB"
must "b back in the host's pair group" \
    sh -c "ifconfig -g pair | grep -qx $PAIRB"
must "destroy after the round trip" ifconfig "$PAIRA" destroy
mustfail "${PAIRB} is gone" ifconfig "$PAIRB"
pass
