#!/bin/sh
# Corner case: the net.link.pair.batch sysctl - default value,
# runtime changes, and the clamp-with-warning for values above the
# queue limit (4096).
. "$(dirname "$0")/lib.sh"
test_init

OID=net.link.pair.batch
orig=$(sysctl -n $OID) || fail "$OID does not exist"
log "ok: $OID exists (value: $orig)"
cleanup_push "sysctl $OID=$orig"

# No assertion on the initial value: a loader.conf tunable or an
# earlier sysctl legitimately changes it; the default (64) is only
# observable on a fresh load.
must "set batch to 32" sysctl "$OID=32"
must "reads back 32" test "$(sysctl -n $OID)" -eq 32
must "0 (yielding disabled) is accepted" sysctl "$OID=0"

must "oversized value is accepted by the handler" sysctl "$OID=8192"
must "and clamped to the queue size" test "$(sysctl -n $OID)" -eq 4096
must_retry 3 "clamp warning reached the console buffer" \
    sh -c "dmesg | tail -5 | grep -q 'if_pair: batch size 8192'"

must "restore original value" sysctl "$OID=$orig"
pass
