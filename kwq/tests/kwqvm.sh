#!/bin/sh
#
# kwqvm.sh - a disposable FreeBSD 15.1/amd64 bhyve guest with a GENERIC-DEBUG
# kernel for testing kwq.ko on this host (nas, FreeBSD 15.0 amd64).
#
# What it does, once (each step is idempotent and can be re-run):
#   fetch    download the official 15.1-RELEASE UFS VM image and verify it
#   image    unpack it into $DISK and grow the file to $DISKSIZE
#   src      clone releng/15.1 into $SRC (objects borrowed from /usr/src)
#   kernel   build kernel-toolchain + $KERNCONF into $OBJ (~30 min on 8 cores);
#            for a second, non-debug kernel beside kernel.debug run
#            KERNCONF=GENERIC INSTKERNNAME=kernel.generic $0 kernel install
#            and pick it at the loader menu (it is in the kernels list)
#   install  mount the image, installkernel as /boot/$INSTKERNNAME, set the
#            loader to boot it, configure serial console, static IP on an
#            isolated tap, root ssh (see keys), crash dumps
#   keys     (part of install) put $AUTHKEYS and the dedicated passphrase-
#            less key $VMKEY (generated if missing) into the guest's
#            /root/.ssh/authorized_keys; needs the guest stopped
#   setup    all of the above in order
# and then, as often as needed:
#   start    create the tap and run the guest in the background (bhyveload
#            + bhyve, restarting on guest reboot); log in $VMDIR/bhyve.log
#   stop     ask the guest to power off over ssh, destroy it after 60 s
#   console  attach to the serial console (cu; ~. to leave); while nobody
#            is attached the console is logged to $VMDIR/console.log
#            (only one reader of the nmdm B side at a time: a cu started
#            by hand also pauses the logger - use 'console')
#   ssh      ssh root@$GUESTIP [command]  (as your user, with $VMKEY)
#   mods     build kwq.ko + kwq_test.ko against the guest kernel (objects
#            in $MODOBJ) and copy them to root@$GUESTIP:/root/kwq/
#            (as your user; runs itself as $DOAS_USER/$SUDO_USER if invoked
#            through doas or sudo, since root on the host has no key)
#   status   show what is running
#   destroy  stop the guest and delete $VMDIR (asks unless -f)
#
# Networking is deliberately isolated: the guest talks to the host only,
# over $TAP ($HOSTIP <-> $GUESTIP).  Nothing on ix0 is touched.  $TAP is
# a vmnet(4) device: a plain tap drops its addresses when bhyve closes
# it, so every guest reboot would cut the host off (that is what
# happened with tap100 on 2026-09-28); the run loop re-adds the address
# before each bhyve start anyway.
#
# Steps that touch the host (fetch, image, src, kernel, install, keys,
# start, stop, console, destroy) need root (doas/sudo); ssh and mods run
# as the invoking user.  Everything configurable is an environment
# variable with a default below.

set -eu

: "${VM:=kwqvm}"
: "${VMBASE:=/bhyve/guests}"
: "${VMDS:=nasssd/bhyve/guests}"		# ZFS parent of $VMBASE; "" = plain dir
: "${DISKSIZE:=20G}"
: "${CPUS:=4}"
: "${MEM:=4G}"
: "${REL:=15.1-RELEASE}"
: "${BRANCH:=releng/15.1}"
: "${SRC:=/bhyve/src-15.1}"
: "${OBJ:=/bhyve/obj-15.1}"
: "${KERNCONF:=GENERIC-DEBUG}"
: "${INSTKERNNAME:=kernel.debug}"
: "${TAP:=vmnet100}"		# vmnet keeps its address across bhyve restarts
: "${HOSTIP:=10.0.99.1}"
: "${GUESTIP:=10.0.99.2}"
: "${KWQSRC:=/home/crest/if_pair/kwq}"
: "${JOBS:=$(sysctl -n hw.ncpu)}"

# The user behind doas/sudo, whose keys and object directory we use.
REALUSER=${DOAS_USER:-${SUDO_USER:-$(id -un)}}
REALHOME=$(eval echo "~$REALUSER")
: "${AUTHKEYS:=$REALHOME/.ssh/authorized_keys}"
: "${VMKEY:=$REALHOME/.ssh/id_kwqvm}"		# passphrase-less, guest only
: "${MODOBJ:=$REALHOME/.kwqvm-obj}"		# module objects, user-writable
SSHOPTS="-i $VMKEY -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$REALHOME/.ssh/known_hosts_kwqvm"

VMDIR=$VMBASE/$VM
DISK=$VMDIR/disk.img
IMG=FreeBSD-$REL-amd64-ufs.raw.xz
URL=https://download.freebsd.org/releases/VM-IMAGES/$REL/amd64/Latest
CON=/dev/nmdm-$VM.1			# ${CON}A: bhyve, ${CON}B: cu
MNT=/mnt/$VM
PIDFILE=$VMDIR/bhyve.pid
LOG=$VMDIR/bhyve.log
CONLOG=$VMDIR/console.log
CONPID=$VMDIR/conlog.pid
KERNOBJ=$OBJ$SRC/amd64.amd64/sys/$KERNCONF
SELF=$(realpath "$0")
export MAKEOBJDIRPREFIX=$OBJ

log()	{ printf '==> %s\n' "$*"; }
die()	{ printf 'kwqvm: %s\n' "$*" >&2; exit 1; }

need_root() {
	[ "$(id -u)" -eq 0 ] || die "run as root (doas $0 $*)"
}

# Re-run this command as $REALUSER when invoked through doas/sudo.
as_user() {	# as_user CMD ARGS...
	if [ "$(id -u)" -eq 0 ] && [ "$REALUSER" != root ]; then
		exec su -m "$REALUSER" -c "exec $SELF $*"
	fi
}

# Generate the dedicated guest key as $REALUSER if it does not exist.
ensure_vmkey() {
	[ -f "$VMKEY.pub" ] && return 0
	log "generating passphrase-less guest key $VMKEY"
	if [ "$(id -u)" -eq 0 ] && [ "$REALUSER" != root ]; then
		su -m "$REALUSER" -c "ssh-keygen -q -t ed25519 -N '' -C kwqvm -f '$VMKEY'"
	else
		ssh-keygen -q -t ed25519 -N '' -C kwqvm -f "$VMKEY"
	fi
}

vm_running() {
	[ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

ensure_vmdir() {
	if [ ! -d "$VMDIR" ]; then
		if [ -n "$VMDS" ] && zfs list -H "$VMDS" >/dev/null 2>&1; then
			log "zfs create $VMDS/$VM"
			zfs create -o mountpoint="$VMDIR" "$VMDS/$VM"
		else
			mkdir -p "$VMDIR"
		fi
	fi
}

# ---------------------------------------------------------------- fetch
cmd_fetch() {
	need_root
	ensure_vmdir
	cd "$VMDIR"
	log "fetching $URL/CHECKSUM.SHA512"
	fetch -q -o CHECKSUM.SHA512 "$URL/CHECKSUM.SHA512"
	want=$(awk -v f="($IMG)" '$2 == f { print $4 }' CHECKSUM.SHA512)
	[ -n "$want" ] || die "no checksum for $IMG in CHECKSUM.SHA512"
	if [ -f "$IMG" ] && [ "$(sha512 -q "$IMG")" = "$want" ]; then
		log "$IMG present and verified"
		return 0
	fi
	log "fetching $URL/$IMG (~640 MB)"
	fetch -o "$IMG" "$URL/$IMG"
	[ "$(sha512 -q "$IMG")" = "$want" ] || die "$IMG: checksum mismatch"
	log "$IMG verified"
}

# ---------------------------------------------------------------- image
cmd_image() {
	need_root
	[ -f "$VMDIR/$IMG" ] || cmd_fetch
	if [ -f "$DISK" ] && [ "${1:-}" != "-f" ]; then
		log "$DISK exists (use 'image -f' to recreate)"
		return 0
	fi
	vm_running && die "guest is running; stop it first"
	log "unpacking $IMG -> $DISK"
	xz -dc "$VMDIR/$IMG" > "$DISK.tmp"
	mv "$DISK.tmp" "$DISK"
	log "growing $DISK to $DISKSIZE (growfs runs on first boot)"
	truncate -s "$DISKSIZE" "$DISK"
}

# ---------------------------------------------------------------- src
cmd_src() {
	need_root
	if [ -d "$SRC/.git" ]; then
		log "$SRC exists ($(git -C "$SRC" describe --tags --always)); pull with: git -C $SRC pull --ff-only"
		return 0
	fi
	log "cloning $BRANCH into $SRC (objects borrowed from /usr/src, then dissociated)"
	git clone --branch "$BRANCH" --single-branch \
	    --reference-if-able /usr/src --dissociate \
	    https://git.freebsd.org/src.git "$SRC"
	log "$SRC at $(git -C "$SRC" describe --tags --always)"
}

# ---------------------------------------------------------------- kernel
cmd_kernel() {
	need_root
	[ -d "$SRC/.git" ] || cmd_src
	[ -f "$SRC/sys/amd64/conf/$KERNCONF" ] || die "$SRC/sys/amd64/conf/$KERNCONF missing"
	mkdir -p "$OBJ"
	if [ -f "$KERNOBJ/kernel" ] && [ "${1:-}" != "-f" ]; then
		log "$KERNOBJ/kernel exists (use 'kernel -f' to rebuild)"
		return 0
	fi
	log "building kernel toolchain in $OBJ (-j$JOBS)"
	make -C "$SRC" -j"$JOBS" -s kernel-toolchain
	log "building $KERNCONF"
	make -C "$SRC" -j"$JOBS" -s buildkernel KERNCONF="$KERNCONF"
	log "kernel: $KERNOBJ/kernel"
}

# ---------------------------------------------------------------- install
mount_image() {
	[ -f "$DISK" ] || die "$DISK missing; run 'image'"
	{ vm_running || bhyve_alive; } && die "guest is running; stop it first"
	MD=$(mdconfig -a -t vnode -f "$DISK")
	part=$(gpart show -p "$MD" | awk '$4 == "freebsd-ufs" { print $3; exit }')
	[ -n "$part" ] || { mdconfig -d -u "$MD"; die "no freebsd-ufs partition in $DISK"; }
	mkdir -p "$MNT"
	mount "/dev/$part" "$MNT"
	log "$DISK mounted at $MNT via /dev/$part"
}

umount_image() {
	umount "$MNT"
	mdconfig -d -u "$MD"
}

# Append a marked block to a file, replacing an earlier block of ours.
set_block() {	# set_block FILE MARK <<EOF ... EOF
	f=$1; mark=$2; tmp=$f.kwqvm
	if [ -f "$f" ]; then
		awk -v m="$mark" '
			$0 == "# BEGIN " m { skip = 1; next }
			$0 == "# END " m   { skip = 0; next }
			!skip' "$f" > "$tmp"
	else
		: > "$tmp"
	fi
	{ echo "# BEGIN $mark"; cat; echo "# END $mark"; } >> "$tmp"
	mv "$tmp" "$f"
}

# Install ssh keys into the mounted image ($MNT).
install_keys() {
	log "sshd: root login with keys from $AUTHKEYS and $VMKEY.pub"
	set_block "$MNT/etc/ssh/sshd_config" kwqvm <<EOF
PermitRootLogin prohibit-password
EOF
	install -d -m 700 "$MNT/root/.ssh" "$MNT/root/kwq"
	{ [ -f "$AUTHKEYS" ] && cat "$AUTHKEYS"; cat "$VMKEY.pub"; } \
	    > "$MNT/root/.ssh/authorized_keys"
	chmod 600 "$MNT/root/.ssh/authorized_keys"
}

cmd_keys() {
	need_root
	ensure_vmkey
	mount_image
	trap 'umount_image' EXIT
	install_keys
	trap - EXIT
	umount_image
	log "keys installed; start the guest and try: $0 ssh uname -a"
}

cmd_install() {
	need_root
	[ -f "$KERNOBJ/kernel" ] || die "$KERNOBJ/kernel missing; run 'kernel'"
	ensure_vmkey
	mount_image
	trap 'umount_image' EXIT

	log "installkernel $KERNCONF -> $MNT/boot/$INSTKERNNAME"
	make -C "$SRC" -s installkernel KERNCONF="$KERNCONF" \
	    INSTKERNNAME="$INSTKERNNAME" DESTDIR="$MNT"

	log "loader.conf: boot $INSTKERNNAME on the serial console"
	set_block "$MNT/boot/loader.conf" kwqvm <<EOF
kernel="$INSTKERNNAME"
kernels="$INSTKERNNAME kernel.generic kernel"
console="comconsole"
boot_serial="YES"
comconsole_speed="115200"
autoboot_delay="3"
kern.msgbufsize="1048576"
debug.debugger_on_panic="0"
EOF

	log "rc.conf: hostname, $GUESTIP on vtnet0, sshd, crash dumps"
	sysrc -q -f "$MNT/etc/rc.conf" \
	    hostname="$VM" \
	    ifconfig_vtnet0="inet $GUESTIP/24" \
	    defaultrouter="$HOSTIP" \
	    sshd_enable="YES" \
	    dumpdev="AUTO" \
	    savecore_enable="YES" \
	    sendmail_enable="NONE" \
	    growfs_enable="YES" >/dev/null
	sysrc -q -f "$MNT/etc/rc.conf" -x ifconfig_DEFAULT 2>/dev/null || true

	install_keys
	printf 'nameserver %s\n' "$HOSTIP" > "$MNT/etc/resolv.conf"
	cat > "$MNT/root/README.kwqvm" <<EOF
kwq test guest.  Kernel: $KERNCONF from $BRANCH as /boot/$INSTKERNNAME
(fallback: /boot/kernel = GENERIC from the release image).  Modules land
in /root/kwq via '$0 mods' on the host.  Crash dumps: /var/crash.
EOF
	trap - EXIT
	umount_image
	log "install done"
}

# ---------------------------------------------------------------- setup
cmd_setup() {
	cmd_fetch
	cmd_image
	cmd_src
	cmd_kernel
	cmd_install
	log "setup complete; next: $0 start; $0 console (or ssh); $0 mods"
}

# ---------------------------------------------------------------- run
ensure_net() {
	kldload -n vmm nmdm if_tap 2>/dev/null || true
	sysctl -q net.link.tap.up_on_open=1 >/dev/null
	ifconfig "$TAP" >/dev/null 2>&1 || ifconfig "$TAP" create
	ifconfig "$TAP" inet "$HOSTIP/24" up
}

# Foreground loop: bhyveload + bhyve; exit 0 from bhyve means "reboot".
cmd__run() {
	while :; do
		ensure_net
		bhyveload -c "${CON}A" -m "$MEM" -d "$DISK" "$VM" || break
		bhyve -c "$CPUS" -m "$MEM" -A -H -P -w \
		    -s 0,hostbridge \
		    -s 3,virtio-blk,"$DISK" \
		    -s 4,virtio-net,"$TAP" \
		    -s 31,lpc -l com1,"${CON}A" \
		    "$VM"
		rc=$?
		bhyvectl --destroy --vm="$VM" 2>/dev/null || true
		[ "$rc" -eq 0 ] || break	# 1 poweroff, 2 halt, >2 error
	done
	rm -f "$PIDFILE"
}

# Console logger: the only reader of the nmdm B side, so it is stopped
# while 'console' has cu attached and restarted afterwards.
conlog_start() {
	conlog_running && return 0
	daemon -f -p "$CONPID" sh -c "exec cat ${CON}B >> $CONLOG"
}
conlog_running() {
	[ -f "$CONPID" ] && kill -0 "$(cat "$CONPID")" 2>/dev/null
}
conlog_stop() {
	conlog_running && kill "$(cat "$CONPID")" 2>/dev/null
	rm -f "$CONPID"
}

cmd_start() {
	need_root
	[ -f "$DISK" ] || die "$DISK missing; run 'setup'"
	vm_running && { log "already running (pid $(cat "$PIDFILE"))"; return 0; }
	ensure_net
	bhyvectl --destroy --vm="$VM" 2>/dev/null || true
	log "starting $VM: $CPUS vCPUs, $MEM, disk $DISK, net $TAP -> $GUESTIP"
	daemon -f -p "$PIDFILE" -o "$LOG" sh "$SELF" _run
	conlog_start
	log "console: $0 console    ssh: $0 ssh    logs: $LOG $CONLOG"
}

bhyve_alive() {
	pgrep -qf "^bhyve: $VM\$"
}

# Ask the guest to power off (so its file system is clean for mount_image);
# fall back to destroying the VM after 60 s or if ssh is not usable.
cmd_stop() {
	need_root
	vm_running || bhyve_alive || { log "not running"; return 0; }
	pid=$(cat "$PIDFILE" 2>/dev/null || echo "")
	log "stopping $VM: asking the guest to power off"
	[ -n "$pid" ] && kill "$pid" 2>/dev/null || true	# no restart loop
	if [ -f "$VMKEY" ]; then
		su -m "$REALUSER" -c "ssh $SSHOPTS -o ConnectTimeout=5 -o BatchMode=yes root@$GUESTIP 'shutdown -p now'" \
		    >/dev/null 2>&1 || true
	fi
	n=0
	while bhyve_alive && [ "$n" -lt 300 ]; do sleep 0.2; n=$((n + 1)); done
	if bhyve_alive; then
		log "guest did not power off in 60 s; destroying it"
		bhyvectl --destroy --vm="$VM" 2>/dev/null || true
		n=0
		while bhyve_alive && [ "$n" -lt 50 ]; do sleep 0.2; n=$((n + 1)); done
	fi
	bhyvectl --destroy --vm="$VM" 2>/dev/null || true
	rm -f "$PIDFILE"
	conlog_stop
	log "$VM stopped"
}

cmd_console() {
	need_root
	log "attaching to ${CON}B; leave with ~. (console.log pauses meanwhile)"
	conlog_stop
	cu -l "${CON}B" -s 115200 || true
	vm_running && conlog_start
}

cmd_ssh() {
	as_user ssh "$@"
	[ -f "$VMKEY" ] || die "$VMKEY missing; run 'doas $0 keys'"
	exec ssh $SSHOPTS "root@$GUESTIP" "$@"
}

cmd_mods() {
	as_user mods
	[ -d "$KERNOBJ" ] || die "$KERNOBJ missing; run 'kernel'"
	[ -f "$VMKEY" ] || die "$VMKEY missing; run 'doas $0 keys'"
	log "building kwq.ko and kwq_test.ko against $KERNCONF ($KERNOBJ)"
	# Module objects go under $MODOBJ, which the user can write (the kernel
	# object tree is root's); make obj creates the directories, without
	# them make would build in the source tree and clobber the host build.
	export MAKEOBJDIRPREFIX=$MODOBJ
	mkdir -p "$MODOBJ"
	make -C "$KWQSRC" -s obj
	make -C "$KWQSRC" -s clean all SYSDIR="$SRC/sys" KERNBUILDDIR="$KERNOBJ"
	mods="$(make -C "$KWQSRC/kwq" -V .OBJDIR)/kwq.ko $(make -C "$KWQSRC/kwq_test" -V .OBJDIR)/kwq_test.ko"
	for m in $mods; do [ -f "$m" ] || die "$m not built"; done
	log "copying to root@$GUESTIP:/root/kwq/"
	scp -q $SSHOPTS $mods "root@$GUESTIP:/root/kwq/"
	log "in the guest: kldload /root/kwq/kwq.ko /root/kwq/kwq_test.ko; sysctl kern.kwq_test"
}

cmd_status() {
	if vm_running; then
		echo "$VM: running, pid $(cat "$PIDFILE"), console ${CON}B, ssh root@$GUESTIP"
		conlog_running && echo "console logged to $CONLOG" || echo "console NOT logged (cu attached?)"
	else
		echo "$VM: not running"
	fi
	[ -f "$DISK" ] && ls -lh "$DISK" | awk '{ print "disk:", $5, $9 }'
	[ -f "$KERNOBJ/kernel" ] && echo "kernel: $KERNOBJ/kernel"
	ifconfig "$TAP" >/dev/null 2>&1 && echo "tap: $TAP $HOSTIP"
	ping -c1 -t1 "$GUESTIP" >/dev/null 2>&1 && echo "guest answers ping" || true
}

cmd_destroy() {
	need_root
	if [ "${1:-}" != "-f" ]; then
		printf 'delete %s (disk, image, logs)? [y/N] ' "$VMDIR"
		read -r ans; [ "$ans" = y ] || exit 1
	fi
	cmd_stop
	ifconfig "$TAP" destroy 2>/dev/null || true
	if [ -n "$VMDS" ] && zfs list -H "$VMDS/$VM" >/dev/null 2>&1; then
		zfs destroy -r "$VMDS/$VM"
	else
		rm -rf "$VMDIR"
	fi
	log "$VMDIR removed ($SRC and $OBJ kept)"
}

usage() {
	sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

[ $# -ge 1 ] || usage
cmd=$1; shift
case $cmd in
fetch|image|src|kernel|install|keys|setup|start|stop|console|ssh|mods|status|destroy)
	"cmd_$cmd" "$@" ;;
_run)	cmd__run ;;
*)	usage ;;
esac
