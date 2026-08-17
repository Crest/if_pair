#!/bin/sh
# Corner cases: explicit unit numbers, duplicate creation, invalid
# clone names, and destroying interfaces that do not exist.
. "$(dirname "$0")/lib.sh"
test_init

U=73
mustfail "unit ${U} is free before the test" ifconfig "pair${U}a"

out=$(ifconfig "pair${U}" create) || fail "explicit-unit create failed"
[ "$out" = "pair${U}a" ] || fail "expected pair${U}a, got '${out}'"
log "ok: explicit-unit create reports pair${U}a"
cleanup_push "ifconfig pair${U}a destroy"
must "pair${U}b exists too" ifconfig "pair${U}b"

mustfail "duplicate unit is refused" ifconfig "pair${U}" create
mustfail "side name is not a clone name" ifconfig "pair${U}a" create
mustfail "junk name is refused" ifconfig pairXY create
mustfail "destroying nonexistent pair9999a" ifconfig pair9999a destroy

must "destroy pair${U}a" ifconfig "pair${U}a" destroy
mustfail "pair${U}b is gone" ifconfig "pair${U}b"
pass
