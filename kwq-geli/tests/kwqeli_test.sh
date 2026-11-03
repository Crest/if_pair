#! /usr/libexec/atf-sh
# ATF test program for kwq-geli (geom_kwqeli.ko on kwq.ko), on md(4)
# providers only.  Run under kyua as root:
#
#   kyua test -k /path/to/kwq-geli/tests/Kyuafile \
#       -v test_suites.kwqeli.moddir=/dir/with/kwq.ko+geom_kwqeli.ko+geom_kwqeli.so
#
# Each case loads kwq.ko and geom_kwqeli.ko unless loaded (and geom_eli.ko
# for the cross-reads), creates its md devices and removes, in cleanup,
# every provider and md device it made and every module it loaded.  The
# knobs a case writes (kern.geom.kwqeli.*, a queue's limit) are put back.

# ---------------------------------------------------------------- helpers

moddir() { atf_config_get moddir "$(atf_get_srcdir)"; }
G() { env GEOM_LIBRARY_PATH="$(moddir)" geom kwqeli "$@"; }
# atf_check execs its command, so it gets the words instead (no spaces in moddir)
GK=""

kwqeli_setup() {
	[ "$(id -u)" -eq 0 ] || atf_skip "needs root"
	local d
	d=$(moddir)
	[ -f "$d/geom_kwqeli.so" ] || atf_skip "no geom_kwqeli.so in $d"
	GK="env GEOM_LIBRARY_PATH=$d geom kwqeli"
	: > loaded; : > mds; : > knobs
	if ! kldstat -q -m kwq; then
		kldload "$d/kwq.ko" || atf_skip "cannot load $d/kwq.ko"
		echo kwq >> loaded
	fi
	if ! kldstat -q -m g_kwqeli; then
		kldload "$d/geom_kwqeli.ko" || atf_skip "cannot load $d/geom_kwqeli.ko"
		echo geom_kwqeli >> loaded
	fi
	dd if=/dev/random of=data.in bs=1m count=48 2>/dev/null
	dd if=/dev/random of=key bs=64 count=1 2>/dev/null
}

# newmd SIZE [TYPE]: an md device, remembered for cleanup
newmd() {
	local m
	m=$(mdconfig -a -t "${2:-malloc}" -s "$1") || atf_fail "mdconfig"
	echo "$m" >> mds
	echo "$m"
}

# knob NAME VALUE: set a sysctl for this case, restored in cleanup
knob() {
	echo "$1=$(sysctl -n "$1")" >> knobs
	sysctl -q "$1=$2" >/dev/null || atf_fail "sysctl $1=$2"
}

kwqeli_cleanup() {
	local m n
	[ -f knobs ] && [ -s knobs ] && sysctl -q $(cat knobs) >/dev/null 2>&1
	if [ -f mds ]; then
		for m in $(cat mds); do
			umount -f "mnt.$m" 2>/dev/null
			G detach -f "$m.kweli" 2>/dev/null
			geli detach -f "$m.eli" 2>/dev/null
		done
		for m in $(cat mds); do
			n=0
			while ! mdconfig -d -u "$m" 2>/dev/null && [ $n -lt 50 ]; do
				sleep 0.1; n=$((n + 1))
			done
		done
	fi
	[ -f loaded ] || return 0
	for m in geom_eli geom_kwqeli kwq; do
		grep -qx $m loaded || continue
		n=0
		while kldstat -q -n $m && ! kldunload $m 2>/dev/null; do
			sleep 0.1; n=$((n + 1))
			[ $n -lt 100 ] || { echo "cannot unload $m" >&2; break; }
		done
	done
	return 0
}

# roundtrip DEV MIB: write MIB of data.in, read it back, compare
roundtrip() {
	dd if=data.in of=data.cmp bs=1m count="$2" 2>/dev/null
	dd if=data.cmp of="/dev/$1" bs=1m 2>/dev/null || atf_fail "write $1"
	dd if="/dev/$1" of=data.out bs=1m count="$2" 2>/dev/null || atf_fail "read $1"
	cmp -s data.cmp data.out || atf_fail "$1: data read back differs"
	rm -f data.out
}

# qsum PROVIDER COUNTER: sum over the CPUs of the provider's kwq queue
qsum() {
	sysctl -a "kern.kwq.bulk.queue.kweli-$1.cpu" 2>/dev/null | \
	    awk -v c="$2" -F': ' '{ n = split($1, p, "."); if (p[n] == c) s += $2 } END { print s + 0 }'
}

# ---------------------------------------------------------------- cases

atf_test_case onetime_xts cleanup
onetime_xts_head() {
	atf_set descr "onetime AES-XTS-256 on 4 KiB sectors: 48 MiB round trip, the bios through the provider's BULK queue"
	atf_set require.user root
}
onetime_xts_body() {
	kwqeli_setup
	m=$(newmd 64m)
	atf_check $GK onetime -e AES-XTS -l 256 -s 4096 "$m"
	roundtrip "$m.kweli" 48
	[ "$(qsum "$m" items)" -gt 0 ] || atf_fail "kern.kwq.bulk.queue.kweli-$m saw no items"
}
onetime_xts_cleanup() { kwqeli_cleanup; }

atf_test_case onetime_cbc cleanup
onetime_cbc_head() {
	atf_set descr "onetime AES-CBC-128 on 512-byte sectors: the SHA256 IV path"
	atf_set require.user root
}
onetime_cbc_body() {
	kwqeli_setup
	m=$(newmd 64m)
	atf_check $GK onetime -e AES-CBC -l 128 "$m"
	roundtrip "$m.kweli" 48
}
onetime_cbc_cleanup() { kwqeli_cleanup; }

atf_test_case onetime_auth cleanup
onetime_auth_head() {
	atf_set descr "onetime HMAC/SHA256 + AES-XTS-128 on 512-byte sectors: integrity, written then read and authenticated"
	atf_set require.user root
}
onetime_auth_body() {
	kwqeli_setup
	m=$(newmd 160m)	# 512-byte sectors with a 32-byte MAC: about half is data
	atf_check $GK onetime -a HMAC/SHA256 -e AES-XTS -l 128 "$m"
	roundtrip "$m.kweli" 48
}
onetime_auth_cleanup() { kwqeli_cleanup; }

atf_test_case cross_geli cleanup
cross_geli_head() {
	atf_set descr "the on-disk format is GELI's: what kwq-geli writes stock geli reads, and the other way round"
	atf_set require.user root
}
cross_geli_body() {
	kwqeli_setup
	if ! kldstat -q -m g_eli; then
		kldload geom_eli || atf_skip "cannot load geom_eli"
		echo geom_eli >> loaded
	fi
	m=$(newmd 64m)
	atf_check -o ignore geli init -B none -P -K key -s 4096 "$m"
	atf_check $GK attach -p -k key "$m"
	dd if=data.in of="/dev/$m.kweli" bs=1m 2>/dev/null
	atf_check $GK detach "$m.kweli"
	atf_check geli attach -p -k key "$m"
	dd if="/dev/$m.eli" of=data.out bs=1m count=48 2>/dev/null
	atf_check geli detach "$m.eli"
	cmp -s data.in data.out || atf_fail "geli does not read what kwq-geli wrote"
	dd if=/dev/random of=data.in2 bs=1m count=48 2>/dev/null
	atf_check geli attach -p -k key "$m"
	dd if=data.in2 of="/dev/$m.eli" bs=1m 2>/dev/null
	atf_check geli detach "$m.eli"
	atf_check $GK attach -p -k key "$m"
	dd if="/dev/$m.kweli" of=data.out bs=1m count=48 2>/dev/null
	cmp -s data.in2 data.out || atf_fail "kwq-geli does not read what geli wrote"
}
cross_geli_cleanup() { kwqeli_cleanup; }

atf_test_case suspend_resume cleanup
suspend_resume_head() {
	atf_set descr "geom kwqeli suspend: new bios park on the softc (no handler sleeps), resume sends them through and the data is intact"
	atf_set require.user root
}
suspend_resume_body() {
	kwqeli_setup
	m=$(newmd 64m)
	atf_check -o ignore $GK init -B none -P -K key -s 4096 "$m"
	atf_check $GK attach -p -k key "$m"
	atf_check $GK suspend "$m.kweli"
	(dd if=data.in of="/dev/$m.kweli" bs=64k 2>/dev/null; echo $? > writer.rc) &
	sleep 1
	[ ! -f writer.rc ] || atf_fail "the writer finished while the provider was suspended"
	# The metadata comes from the provider below: the suspended one would
	# park the read like any other.
	atf_check $GK resume -p -k key "$m"
	wait
	atf_check_equal 0 "$(cat writer.rc)"
	dd if="/dev/$m.kweli" of=data.out bs=1m count=48 2>/dev/null
	cmp -s data.in data.out || atf_fail "data after suspend and resume differs"
}
suspend_resume_cleanup() { kwqeli_cleanup; }

atf_test_case providers cleanup
providers_head() {
	atf_set descr "eight providers written and read at once, each with its own queue"
	atf_set require.user root
}
providers_body() {
	kwqeli_setup
	list=""
	for i in 1 2 3 4 5 6 7 8; do
		m=$(newmd 40m)
		atf_check $GK onetime -s 4096 "$m"
		list="$list $m"
	done
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	for m in $list; do
		(dd if=data.cmp of="/dev/$m.kweli" bs=1m 2>/dev/null &&
		    dd if="/dev/$m.kweli" of="data.out.$m" bs=1m count=32 2>/dev/null) &
	done
	wait
	for m in $list; do
		cmp -s data.cmp "data.out.$m" || atf_fail "$m: data differs"
		[ "$(qsum "$m" items)" -gt 0 ] || atf_fail "$m: no items on its queue"
	done
}
providers_cleanup() { kwqeli_cleanup; }

atf_test_case enobufs_retry cleanup
enobufs_retry_head() {
	atf_set descr "a queue limit of 1 refuses bios (ENOBUFS), GEOM retries them as ENOMEM, and the data still round-trips"
	atf_set require.user root
}
enobufs_retry_body() {
	kwqeli_setup
	knob kern.geom.kwqeli.cpu 1		# every bio onto one list
	m=$(newmd 64m)
	atf_check $GK onetime -s 4096 "$m"
	atf_check -o ignore sysctl "kern.kwq.bulk.queue.kweli-$m.limit=1"
	nomem0=$(sysctl -n kern.geom.nomem_count)
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	for i in 0 1 2 3; do
		dd if=data.cmp of="/dev/$m.kweli" bs=64k skip=$((i * 128)) \
		    seek=$((i * 128)) count=128 2>/dev/null &
	done
	wait
	dd if="/dev/$m.kweli" of=data.out bs=1m count=32 2>/dev/null
	cmp -s data.cmp data.out || atf_fail "data differs after refusals"
	echo "rejected $(qsum "$m" rejected), nomem_count +$(( $(sysctl -n kern.geom.nomem_count) - nomem0 ))"
	[ "$(qsum "$m" rejected)" -gt 0 ] || atf_skip "the load never filled a list of 1 (nothing to check)"
}
enobufs_retry_cleanup() { kwqeli_cleanup; }

atf_test_case steal cleanup
steal_head() {
	atf_set descr "every bio of a CPU-heavy (HMAC/SHA256) provider queued to one CPU of a stealable queue: other BULK workers steal a tenth or more and the data is intact"
	atf_set require.user root
}
steal_body() {
	kwqeli_setup
	knob kern.geom.kwqeli.cpu 1
	knob kern.geom.kwqeli.stealable 1
	m=$(newmd 320m)
	# AES-XTS alone keeps up with four writers on one CPU (its list never
	# holds a second bio); with the MAC it falls behind.
	atf_check $GK onetime -a HMAC/SHA256 -s 4096 "$m"
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	for i in 0 1 2 3; do
		dd if=data.cmp of="/dev/$m.kweli" bs=1m seek=$((i * 32)) 2>/dev/null &
	done
	wait
	for i in 0 1 2 3; do
		dd if="/dev/$m.kweli" of=data.out bs=1m skip=$((i * 32)) count=32 2>/dev/null
		cmp -s data.cmp data.out || atf_fail "range $i differs"
	done
	echo "steals_in $(qsum "$m" steals_in) stolen_in $(qsum "$m" stolen_in) steals_out $(qsum "$m" steals_out)"
	atf_check_equal "$(qsum "$m" steals_in)" "$(qsum "$m" steals_out)"
	[ $(( $(qsum "$m" stolen_in) * 10 )) -ge "$(qsum "$m" items)" ] || \
	    atf_fail "$(qsum "$m" stolen_in) of $(qsum "$m" items) bios stolen, wanted a tenth"
}
steal_cleanup() { kwqeli_cleanup; }

atf_test_case key_cache cleanup
key_cache_head() {
	atf_set descr "a key cache of 2 keys on a provider of 8: keys replaced in the BULK handler without sleeping, data intact"
	atf_set require.user root
}
key_cache_body() {
	kwqeli_setup
	if grep -qx geom_kwqeli loaded; then
		kldunload geom_kwqeli || atf_skip "cannot reload geom_kwqeli"
		sed -i '' '/^geom_kwqeli$/d' loaded
	else
		atf_skip "geom_kwqeli was loaded before the case: cannot set its tunable"
	fi
	kenv kern.geom.kwqeli.key_cache_limit=2 >/dev/null
	kldload "$(moddir)/geom_kwqeli.ko"
	echo geom_kwqeli >> loaded
	kenv -u kern.geom.kwqeli.key_cache_limit
	atf_check_equal 2 "$(sysctl -n kern.geom.kwqeli.key_cache_limit)"
	# A key per 2^20 sectors of 512 bytes: 512 MiB each; swap-backed, so
	# only the written ranges take memory.
	m=$(newmd 4g swap)
	atf_check $GK onetime -e AES-XTS -l 128 "$m"
	dd if=data.in of=data.cmp bs=1m count=4 2>/dev/null
	for k in 0 1 2 3 4 5 6 7; do
		dd if=data.cmp of="/dev/$m.kweli" bs=1m seek=$((k * 512)) 2>/dev/null || atf_fail "write key $k"
	done
	for k in 7 3 0 5 1 6 2 4; do
		dd if="/dev/$m.kweli" of=data.out bs=1m skip=$((k * 512)) count=4 2>/dev/null
		cmp -s data.cmp data.out || atf_fail "key range $k differs"
	done
	G list "$m.kweli" | grep -E "KeysAllocated|KeysTotal"
	[ "$(sysctl -n kern.geom.kwqeli.key_cache_misses)" -gt 0 ] || atf_fail "no key cache miss"
}
key_cache_cleanup() { kwqeli_cleanup; }

atf_test_case ufs cleanup
ufs_head() {
	atf_set descr "UFS on a provider: newfs, files through the buffer cache (unmapped bios), umount, fsck clean"
	atf_set require.user root
}
ufs_body() {
	kwqeli_setup
	m=$(newmd 256m)
	atf_check $GK onetime -s 4096 "$m"
	atf_check -o ignore newfs -U "/dev/$m.kweli"
	mkdir "mnt.$m"
	atf_check mount "/dev/$m.kweli" "mnt.$m"
	for i in 1 2 3 4; do cp data.in "mnt.$m/f$i"; done
	atf_check umount "mnt.$m"
	atf_check -o ignore fsck -t ufs -n "/dev/$m.kweli"
	atf_check mount -o ro "/dev/$m.kweli" "mnt.$m"
	for i in 1 2 3 4; do cmp -s data.in "mnt.$m/f$i" || atf_fail "f$i differs"; done
	atf_check umount "mnt.$m"
}
ufs_cleanup() { kwqeli_cleanup; }

atf_test_case detach_load cleanup
detach_load_head() {
	atf_set descr "a forced detach under a writer: the writer gets errors, the queue drains, the module unloads"
	atf_set require.user root
}
detach_load_body() {
	kwqeli_setup
	m=$(newmd 64m)
	atf_check $GK onetime -s 4096 "$m"
	(dd if=/dev/zero of="/dev/$m.kweli" bs=64k count=100000 2>/dev/null; true) &
	sleep 0.5
	atf_check $GK detach -f "$m.kweli"
	wait
	[ ! -e "/dev/$m.kweli" ] || atf_fail "provider still there"
	atf_check_equal 0 "$(sysctl -a kern.kwq.bulk.queue 2>/dev/null | grep -c "kweli-$m\.")"
}
detach_load_cleanup() { kwqeli_cleanup; }

atf_test_case resume cleanup
resume_head() {
	atf_set descr "bios cut part way (all on one CPU beside a spinning user process, so the share limit ends passes mid-bio) go on where they stopped: stock geli reads back exactly what kwq-geli wrote"
	atf_set require.user root
}
resume_body() {
	kwqeli_setup
	if ! kldstat -q -m g_eli; then
		kldload geom_eli || atf_skip "cannot load geom_eli"
		echo geom_eli >> loaded
	fi
	knob kern.geom.kwqeli.cpu 1
	knob kern.geom.kwqeli.stealable 0
	cc -O2 -o spin -x c - <<'SPIN' || atf_skip "no cc"
#include <time.h>
int main(void) { struct timespec a, b; volatile unsigned long x = 0;
	clock_gettime(CLOCK_MONOTONIC, &a);
	do { for (int i = 0; i < 100000; i++) x += i; clock_gettime(CLOCK_MONOTONIC, &b); }
	while (b.tv_sec - a.tv_sec < 20); return (0); }
SPIN
	list=""
	for i in 1 2 3 4; do
		m=$(newmd 64m)
		atf_check -o ignore geli init -B none -P -K key -s 4096 "$m"
		atf_check $GK attach -p -k key "$m"
		list="$list $m"
	done
	cpuset -l 1 ./spin & spid=$!
	sleep 0.5
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	for m in $list; do dd if=data.cmp of="/dev/$m.kweli" bs=1m 2>/dev/null & done
	wait $(jobs -p | grep -v "^$spid\$") 2>/dev/null
	for m in $list; do dd if="/dev/$m.kweli" of="data.r.$m" bs=1m count=32 2>/dev/null; done
	kill $spid 2>/dev/null; wait
	req=0
	for m in $list; do req=$((req + $(qsum "$m" requeued))); done
	echo "requeued $req"
	for m in $list; do
		cmp -s data.cmp "data.r.$m" || atf_fail "$m: kwq-geli read back something else"
		atf_check $GK detach "$m.kweli"
		atf_check geli attach -p -k key "$m"
		dd if="/dev/$m.eli" of=data.out bs=1m count=32 2>/dev/null
		atf_check geli detach "$m.eli"
		cmp -s data.cmp data.out || atf_fail "$m: stock geli reads something else"
	done
	[ "$req" -gt 0 ] || atf_skip "no bio was cut part way (requeued 0): nothing was resumed"
}
resume_cleanup() { kwqeli_cleanup; }

atf_test_case nomem cleanup
nomem_head() {
	atf_set descr "crypto requests that cannot be had (fail point kwqeli_getreq, 0.2 % of sectors) fail bios part way with ENOMEM, GEOM issues them again, and stock geli reads back exactly what kwq-geli wrote"
	atf_set require.user root
}
nomem_body() {
	kwqeli_setup
	if ! kldstat -q -m g_eli; then
		kldload geom_eli || atf_skip "cannot load geom_eli"
		echo geom_eli >> loaded
	fi
	m=$(newmd 64m)
	atf_check -o ignore geli init -B none -P -K key -s 4096 "$m"
	atf_check $GK attach -p -k key "$m"
	nomem0=$(sysctl -n kern.geom.nomem_count)
	knob debug.fail_point.kwqeli_getreq '0.2%return'
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	dd if=data.cmp of="/dev/$m.kweli" bs=1m 2>/dev/null || atf_fail "write"
	w=$(( $(sysctl -n kern.geom.nomem_count) - nomem0 ))
	dd if="/dev/$m.kweli" of=data.out bs=1m count=32 2>/dev/null || atf_fail "read"
	r=$(( $(sysctl -n kern.geom.nomem_count) - nomem0 - w ))
	sysctl -q debug.fail_point.kwqeli_getreq=off >/dev/null
	echo "nomem_count +$w writing, +$r reading"
	cmp -s data.cmp data.out || atf_fail "kwq-geli read back something else"
	atf_check $GK detach "$m.kweli"
	atf_check geli attach -p -k key "$m"
	dd if="/dev/$m.eli" of=data.out bs=1m count=32 2>/dev/null
	atf_check geli detach "$m.eli"
	cmp -s data.cmp data.out || atf_fail "stock geli reads something else"
	[ "$w" -gt 0 ] && [ "$r" -gt 0 ] || \
	    atf_skip "no ENOMEM in one direction (write $w, read $r): asynchronous crypto?"
}
nomem_cleanup() { kwqeli_cleanup; }

atf_test_case resume_fail cleanup
resume_fail_head() {
	atf_set descr "a resume whose second session cannot be made (fail point kwqeli_resume_session) fails and leaves the provider suspended, its parked bios waiting; the next resume sends them through"
	atf_set require.user root
}
resume_fail_body() {
	kwqeli_setup
	[ "$(sysctl -n kern.smp.cpus)" -ge 2 ] || atf_skip "needs two CPUs"
	m=$(newmd 64m)
	atf_check -o ignore $GK init -B none -P -K key -s 4096 "$m"
	atf_check $GK attach -p -k key "$m"
	dd if=data.in of=data.cmp bs=1m count=32 2>/dev/null
	dd if=data.cmp of="/dev/$m.kweli" bs=1m 2>/dev/null || atf_fail "write"
	atf_check $GK suspend "$m.kweli"
	(dd if="/dev/$m.kweli" of=data.out bs=1m count=32 2>/dev/null; echo $? > reader.rc) &
	knob debug.fail_point.kwqeli_resume_session '1*off->1*return(12)'
	atf_check -s not-exit:0 -e match:"still suspended" $GK resume -p -k key "$m"
	sleep 1
	[ ! -f reader.rc ] || atf_fail "the reader finished after the failed resume"
	$GK list "$m.kweli" | grep -q "SUSPEND" || atf_fail "not suspended after the failed resume"
	sysctl -q debug.fail_point.kwqeli_resume_session=off >/dev/null
	atf_check $GK resume -p -k key "$m"
	wait
	atf_check_equal 0 "$(cat reader.rc)"
	cmp -s data.cmp data.out || atf_fail "data after the second resume differs"
}
resume_fail_cleanup() { kwqeli_cleanup; }

atf_init_test_cases() {
	atf_add_test_case onetime_xts
	atf_add_test_case onetime_cbc
	atf_add_test_case onetime_auth
	atf_add_test_case cross_geli
	atf_add_test_case suspend_resume
	atf_add_test_case providers
	atf_add_test_case enobufs_retry
	atf_add_test_case steal
	atf_add_test_case key_cache
	atf_add_test_case ufs
	atf_add_test_case detach_load
	atf_add_test_case resume
	atf_add_test_case nomem
	atf_add_test_case resume_fail
}
