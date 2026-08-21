# lib.sh - shared helpers for the if_pair test scripts.  POSIX sh.
#
# The tests touch runtime state only: the if_pair module, pair
# interfaces, and persistent vnet jails named ifp_* with path=/ (they
# share the host file system; nothing is written into them).  No
# configuration files are modified.
#
# Each test sources this file, calls test_init, runs its checks with
# must/mustfail/must_retry, and finishes with pass.  pass undoes
# everything registered with cleanup_push (LIFO, best effort) and
# exits 0.  Any failed check exits 1 immediately, leaving all state
# in place for debugging; cleanup.sh sweeps it once you are done.
#
# Environment: IFPAIR_KO may point at the module to load; default is
# if_pair.ko next to or above the test directory.

TEST=$(basename "$0" .sh)
TDIR=$(dirname "$0")
JPREFIX="ifp_${TEST#t_}"
CLEANUP=""
MOD_PRELOADED=no

log() {
	echo "${TEST}: $*"
}

fail() {
	echo "${TEST}: FAIL: $*" >&2
	echo "${TEST}: state left in place for debugging; inspect with" >&2
	echo "    jls; ifconfig -g pair; kldstat -m if_pair" >&2
	echo "  and sweep with 'sh ${TDIR}/cleanup.sh' when done." >&2
	exit 1
}

cleanup_push() {
	CLEANUP="$*
${CLEANUP}"
}

pass() {
	printf '%s\n' "${CLEANUP}" | while IFS= read -r _cmd; do
		[ -n "$_cmd" ] && eval "$_cmd" >/dev/null 2>&1
	done
	log "PASS"
	exit 0
}

# must "description" command...: the command must succeed.
must() {
	_desc=$1; shift
	if _out=$("$@" 2>&1); then
		log "ok: ${_desc}"
	else
		[ -n "$_out" ] && echo "$_out" >&2
		fail "${_desc} ('$*' failed)"
	fi
}

# mustfail "description" command...: the command must fail.
mustfail() {
	_desc=$1; shift
	if _out=$("$@" 2>&1); then
		fail "${_desc} ('$*' unexpectedly succeeded)"
	else
		log "ok: ${_desc}"
	fi
}

# must_retry tries "description" command...: retry once per second;
# for things that settle asynchronously (DAD, jail teardown).
must_retry() {
	_n=$1; _desc=$2; shift 2
	_i=0
	while :; do
		if _out=$("$@" 2>&1); then
			log "ok: ${_desc}"
			return 0
		fi
		_i=$((_i + 1))
		if [ "$_i" -ge "$_n" ]; then
			[ -n "$_out" ] && echo "$_out" >&2
			fail "${_desc} (no success in ${_n} tries)"
		fi
		sleep 1
	done
}

resolve_ko() {
	for _k in "${IFPAIR_KO:-}" "${TDIR}/../if_pair.ko" \
	    "${TDIR}/if_pair.ko"; do
		if [ -n "$_k" ] && [ -f "$_k" ]; then
			echo "$_k"
			return 0
		fi
	done
	return 1
}

ensure_module() {
	if kldstat -q -m if_pair; then
		MOD_PRELOADED=yes
		return 0
	fi
	_ko=$(resolve_ko) || \
	    fail "if_pair.ko not found (build it or set IFPAIR_KO)"
	must "load ${_ko}" kldload "$_ko"
	cleanup_push "kldunload if_pair"
}

test_init() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "${TEST}: must run as root" >&2
		exit 1
	fi
	ensure_module
}

# create_pair: sets PAIRA and PAIRB, schedules destruction for pass.
create_pair() {
	PAIRA=$(ifconfig pair create) || fail "'ifconfig pair create' failed"
	case "$PAIRA" in
	pair*a)	;;
	*)	fail "unexpected name from create: '${PAIRA}'" ;;
	esac
	PAIRB="${PAIRA%a}b"
	cleanup_push "ifconfig ${PAIRA} destroy"
	log "created ${PAIRA} + ${PAIRB}"
}

jname() {
	echo "${JPREFIX}_$1"
}

# ifdrops jail iface: print the interface's input-discard and
# output-queue-drop counters as "<idrop> <oqdrops>", taken from the
# netstat link row.  Fields are addressed from the end of the line
# because the address column is empty on point-to-point interfaces.
ifdrops() {
	jexec "$1" netstat -I "$2" -dn | awk '
	    /<Link/ { print $(NF - 4), $NF; found = 1; exit }
	    END { if (!found) print "0 0" }'
}

# mkjail name: persistent vnet jail sharing the host file system.
mkjail() {
	if jls -j "$1" jid >/dev/null 2>&1; then
		fail "jail $1 already exists (leftover state? run cleanup.sh)"
	fi
	must "create jail $1" jail -c "name=$1" path=/ vnet=new persist
	cleanup_push "jail -r $1"
}
