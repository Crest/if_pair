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

# skip "reason": like pass, but the test did not apply here.  Runs the
# cleanups too, so a skip after test_init leaves no module loaded.
skip() {
	printf '%s\n' "${CLEANUP}" | while IFS= read -r _cmd; do
		[ -n "$_cmd" ] && eval "$_cmd" >/dev/null 2>&1
	done
	log "SKIP: $1"
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

# Which driver the tests exercise.  PAIR is the cloner (interfaces
# ${PAIR}<N>a/b), PAIR_MOD the module, PAIR_OID its sysctl node.  The
# defaults are if_pair; PAIR=pairtq PAIR_MOD=if_pair_tq selects the
# frozen taskqueue baseline in extras/if_pair_tq, so the same benchmark
# runs against both drivers, even loaded side by side.
: "${PAIR:=pair}"
: "${PAIR_MOD:=if_pair}"
PAIR_OID=net.link.${PAIR}

resolve_ko() {
	for _k in "${IFPAIR_KO:-}" "${TDIR}/../${PAIR_MOD}.ko" \
	    "${TDIR}/${PAIR_MOD}.ko" "${TDIR}/../extras/${PAIR_MOD}/${PAIR_MOD}.ko"; do
		if [ -n "$_k" ] && [ -f "$_k" ]; then
			echo "$_k"
			return 0
		fi
	done
	return 1
}

# if_pair depends on kwq.ko; the taskqueue baseline does not.  Load it
# from KWQ_KO, the source tree, or by name (installed in /boot/modules)
# and unload it after the driver.
ensure_kwq() {
	[ "${PAIR_MOD}" = if_pair ] || return 0
	kldstat -q -m kwq && return 0
	for _k in "${KWQ_KO:-}" "${TDIR}/../kwq/kwq/kwq.ko" "${TDIR}/kwq.ko" kwq; do
		[ -n "$_k" ] || continue
		if kldload "$_k" 2>/dev/null; then
			cleanup_push "kldunload kwq"
			return 0
		fi
	done
	fail "kwq.ko not found (build kwq/ or set KWQ_KO)"
}

ensure_module() {
	if kldstat -q -m "${PAIR_MOD}"; then
		# ifconfig(8) autoloads if_<cloner>.ko from the module path
		# when a cloner is unknown, so a preloaded if_pair may be an
		# installed copy rather than the one built here.  The kwq
		# driver cannot be loaded without kwq.ko: its absence means
		# the taskqueue version is what is running.
		if [ "${PAIR_MOD}" = if_pair ] && ! kldstat -q -m kwq; then
			fail "a preloaded if_pair without kwq.ko: the installed taskqueue driver, not the kwq one (kldunload if_pair, or set PAIR_MOD)"
		fi
		MOD_PRELOADED=yes
		return 0
	fi
	ensure_kwq
	_ko=$(resolve_ko) || \
	    fail "${PAIR_MOD}.ko not found (build it or set IFPAIR_KO)"
	must "load ${_ko}" kldload "$_ko"
	cleanup_push "kldunload ${PAIR_MOD}"
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
	PAIRA=$(ifconfig "${PAIR}" create) || fail "'ifconfig ${PAIR} create' failed"
	case "$PAIRA" in
	${PAIR}*a)	;;
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
