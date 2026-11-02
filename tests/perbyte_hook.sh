#!/bin/sh
# perbyte_hook.sh - a TPUT1_HOOK for compare.sh: what one single flow costs
# per byte, for WAKE.md S12.2's slow mode (the load generators as busy, at
# the same clock, moving 10 % less data).  Run as root by compare.sh:
#
#   perbyte_hook.sh pre|start|stop DIR CPID SPID SECS JC JS WCPU REP
#
# pre (before the client starts) and stop (after it exits) snapshot both
# jails' TCP counters and interface counters and the server's rusage;
# start attaches hwpmc counting to the iperf3 client and server processes
# (PB_EVENTS, default below; a cumulative row a second, the last one the
# total, since a busy thread's count is only refreshed when it switches
# out), takes rusage and
# netstat -x of the connection at a quarter and three quarters of the run,
# and by REP mod 3 adds nothing (1), lockstat -C (2) or a DTrace profile of
# client, server and W's CPU with the data segments' sizes and windows, who
# sends the ACKs, and the generators' switches and migrations (0).  Every
# probe ends before the client does; stop waits for them, so no consumer
# outlives the run (a live dtrace vetoes compare.sh's module unloads).
PH=$1; DIR=$2; CPID=$3; SPID=$4; SECS=$5; JC=$6; JS=$7; WCPU=$8; REP=$9
# Zen 3 has six counters, three per process: cycles, instructions and, by
# REP / 3 mod 2, fills from L3 or another core's L2 or fills from DRAM.
case $(( (REP / 3) % 2 )) in
0) _fill=ls_any_fills_from_sys.int_cache ;;
*) _fill=ls_any_fills_from_sys.mem_io_local ;;
esac
[ -n "${PB_FILL:-}" ] && _fill=$PB_FILL
EV=${PB_EVENTS:-"ls_not_halted_cyc ex_ret_instr $_fill"}
# PB_ROTATE=no keeps every repetition plain (lockstat and DTrace both put
# lab's single flow into its slow mode by their own cost).  PB_BUFCAP=N
# caps both jails' TCP buffer autotuning at N bytes on even repetitions
# (net.inet.tcp.{send,recv}buf_max are per vnet), the A/B for the buffer
# working set against the L3.
ROTATE=${PB_ROTATE:-yes}
PORT=${PB_PORT:-5201}

snap() {	# suffix
	vmstat -z | grep -E "^ITEM|^mbuf" > "$DIR/z.$1" 2>&1
	jexec "$JC" netstat -s -p tcp > "$DIR/tcp_c.$1" 2>&1
	jexec "$JS" netstat -s -p tcp > "$DIR/tcp_s.$1" 2>&1
	jexec "$JC" netstat -ibnW > "$DIR/if_c.$1" 2>&1
	jexec "$JS" netstat -ibnW > "$DIR/if_s.$1" 2>&1
	procstat -r "$SPID" > "$DIR/ru_s.$1" 2>&1
}
mid() {	# suffix: rusage of both, the connection's buffers, the mbuf zones
	vmstat -z | grep -E "^ITEM|^mbuf" > "$DIR/z.$1" 2>&1
	procstat -r "$CPID" > "$DIR/ru_c.$1" 2>&1
	procstat -r "$SPID" > "$DIR/ru_s.$1" 2>&1
	jexec "$JC" netstat -xn -p tcp > "$DIR/x_c.$1" 2>&1
	jexec "$JS" netstat -xn -p tcp > "$DIR/x_s.$1" 2>&1
	jexec "$JC" netstat -nT -p tcp > "$DIR/t_c.$1" 2>&1
	jexec "$JS" netstat -nT -p tcp > "$DIR/t_s.$1" 2>&1
}
profile() {	# the DTrace probe of REP mod 3 = 0
	cat > "$DIR/pb.d" <<EOF
self int lc;
self uint64_t off;
profile-1999 /arg0 && (pid == $CPID || pid == $SPID || cpu == $WCPU)/
{ @k[pid == $CPID ? "cli" : pid == $SPID ? "srv" : "w", func(arg0)] = count(); }
profile-1999 /arg1 && (pid == $CPID || pid == $SPID || cpu == $WCPU)/
{ @u[pid == $CPID ? "cli" : pid == $SPID ? "srv" : "w"] = count(); }
profile-1999 /arg0 && (pid == $CPID || pid == $SPID)/
{ @st[pid == $CPID ? "cli" : "srv", stack(6)] = count(); }
tcp:::send /args[4]->tcp_dport == $PORT/
{ @len["data segment ip_plength"] = quantize(args[2]->ip_plength);
  @win["cwnd"] = quantize(args[3]->tcps_cwnd); @win["swnd (peer's window)"] = quantize(args[3]->tcps_swnd);
  @who["data sent by", pid == $CPID ? "cli" : cpu == $WCPU ? "w" : "other"] = count(); }
tcp:::send /args[4]->tcp_sport == $PORT/
{ @len["ack ip_plength"] = quantize(args[2]->ip_plength);
  @win["rwnd (advertised)"] = quantize(args[3]->tcps_rwnd);
  @who["ack sent by", pid == $SPID ? "srv" : cpu == $WCPU ? "w" : "other"] = count(); }
sched:::on-cpu /(pid == $CPID || pid == $SPID) && self->lc && self->lc != cpu + 1/
{ @sw["migrations", pid == $CPID ? "cli" : "srv", tid] = count(); }
sched:::on-cpu /(pid == $CPID || pid == $SPID) && self->off/
{ @offt[pid == $CPID ? "cli" : "srv", tid] = sum(timestamp - self->off); }
sched:::on-cpu /pid == $CPID || pid == $SPID/
{ @sw["on-cpu", pid == $CPID ? "cli" : "srv", tid] = count(); self->lc = cpu + 1; self->off = 0; }
sched:::off-cpu /pid == $CPID || pid == $SPID/ { self->off = timestamp; }
tick-$((SECS - 3))s
{ trunc(@st, 40); printa("%s %a %@d\n", @k); printa("user %s %@d\n", @u);
  printa("%s %s %@d\n", @who); printa("%s %s %d %@d\n", @sw); printa("offcpu_ns %s %d %@d\n", @offt);
  printa(@len); printa(@win); printa("stack %s %k %@d\n", @st); exit(0); }
EOF
	dtrace -q -x aggsize=16m -x dynvarsize=8m -o "$DIR/dtrace" -s "$DIR/pb.d" > "$DIR/dtrace.err" 2>&1 &
	echo $! > "$DIR/probe.pid"
}

dlight() {	# PB_DLIGHT=yes: why the generators sleep and who wakes them
	cat > "$DIR/pbl.d" <<EOF
sched:::off-cpu /pid == $CPID || pid == $SPID/
{ @wm[pid == $CPID ? "cli" : "srv", curthread->td_wmesg != 0 ? stringof(curthread->td_wmesg) :
      (curthread->td_lockname != 0 ? stringof(curthread->td_lockname) : "-")] = count();
  @os[pid == $CPID ? "cli" : "srv", stack(8)] = count(); }
sched:::wakeup /args[1]->p_pid == $CPID || args[1]->p_pid == $SPID/
{ @wk[args[1]->p_pid == $CPID ? "cli" : "srv", cpu == $WCPU ? "W cpu" : pid == $CPID ? "cli" : pid == $SPID ? "srv" : execname] = count();
  @ws[args[1]->p_pid == $CPID ? "cli" : "srv", stack(8)] = count(); }
tick-$((SECS - 3))s
{ trunc(@os, 12); trunc(@ws, 12);
  printa("offcpu %s %s %@d\n", @wm); printa("wokenby %s %s %@d\n", @wk);
  printa("offstack %s %k %@d\n", @os); printa("wakestack %s %k %@d\n", @ws); exit(0); }
EOF
	dtrace -q -x aggsize=8m -o "$DIR/dlight" -s "$DIR/pbl.d" > "$DIR/dlight.err" 2>&1 &
	echo $! > "$DIR/probe.pid"
}

dfree() {	# PB_DFREE=yes: which CPUs return 4 KB jumbo pages to UMA, per second, and from where
	cat > "$DIR/pbf.d" <<EOF
BEGIN { start = timestamp; }
fbt::uma_zfree_arg:entry /arg0 == (uintptr_t)\`zone_jumbop/
{ @s[cpu, (timestamp - start) / 1000000000] = count(); @st[cpu, stack(6)] = count(); }
tick-$((SECS - 3))s
{ trunc(@st, 8); printa("free %d %d %@d\n", @s); printa("stack %d %k %@d\n", @st); exit(0); }
EOF
	dtrace -q -x aggsize=8m -o "$DIR/dfree" -s "$DIR/pbf.d" > "$DIR/dfree.err" 2>&1 &
	echo $! > "$DIR/probe.pid"
}

dreuse() {	# PB_DREUSE=yes: a jumbo page's reuse distance, in jumbo pages freed between its free and its next allocation
	cat > "$DIR/pbr.d" <<EOF
BEGIN { nfree = 0; }
fbt::uma_zfree_arg:entry /arg0 == (uintptr_t)\`zone_jumbop/
{ nfree++; seq[arg1] = nfree; fcpu[arg1] = cpu; }
fbt::uma_zalloc_arg:entry /arg0 == (uintptr_t)\`zone_jumbop/ { self->z = 1; }
fbt::uma_zalloc_arg:return /self->z && seq[arg1] != 0/
{ @d = quantize(nfree - seq[arg1]); @dc[fcpu[arg1]] = quantize(nfree - seq[arg1]); @n["reused"] = count(); seq[arg1] = 0; self->z = 0; }
fbt::uma_zalloc_arg:return /self->z/ { @n["first seen"] = count(); self->z = 0; }
tick-$((SECS - 3))s
{ printa("reuse %@d\n", @d); printa("reuse_by_freeing_cpu %d %@d\n", @dc); printa("count %s %@d\n", @n); exit(0); }
EOF
	dtrace -q -x dynvarsize=32m -x aggsize=8m -o "$DIR/dreuse" -s "$DIR/pbr.d" > "$DIR/dreuse.err" 2>&1 &
	echo $! > "$DIR/probe.pid"
}

case $PH in
pre)
	mkdir -p "$DIR"
	if [ -n "${PB_BUFCAP:-}" ] && [ $((REP % 2)) -eq 0 ]; then
		for _j in "$JC" "$JS"; do
			jexec "$_j" sysctl -q net.inet.tcp.sendbuf_max="$PB_BUFCAP" net.inet.tcp.recvbuf_max="$PB_BUFCAP" > /dev/null
		done
		echo "cap $PB_BUFCAP" > "$DIR/arm"
	else
		echo "default" > "$DIR/arm"
	fi
	jexec "$JC" sysctl net.inet.tcp.sendbuf_max net.inet.tcp.recvbuf_max >> "$DIR/arm" 2>&1
	# PB_STEAL_AB=yes runs even repetitions with ULE's idle steal off
	# (kern.sched.ule.steal_idle=0, a host setting crest allowed for this
	# A/B on 2026-10-29); stop restores PB_STEAL_ORIG, and the runner
	# restores it again on exit.
	if [ "${PB_STEAL_AB:-no}" = yes ] && [ $((REP % 2)) -eq 0 ]; then
		sysctl -q kern.sched.ule.steal_idle=0 > /dev/null
	fi
	echo "steal_idle $(sysctl -n kern.sched.ule.steal_idle)" >> "$DIR/arm"
	snap 0 ;;
start)
	# PB_THR=US samples both generators' threads every US microseconds
	# with thrsample(1) (state, wait message or lock, CPU), which hooks
	# nothing in the scheduler; started at once so that its clock is the
	# client's, as iperf3's intervals are.
	if [ -n "${PB_THR:-}" ]; then
		"$(dirname "$0")/thrsample" "$PB_THR" "$((SECS + 1))" "$CPID" "$SPID" > "$DIR/thr.txt" 2>&1 &
		echo $! > "$DIR/thr.pid"
	fi
	# PB_CTR=US samples counters every US microseconds with ctrsample(1),
	# on the same clock: every CPU's cp_times, W's kwq worker counters, the
	# items, doorbells and skipped doorbells of every kwq queue on W's CPU,
	# the UMA zones of the data pages (mbuf_jumbo_page) and the mbufs
	# (allocations, frees, in use, the zone's bucket cache and its working
	# set estimate, the keg's pages and free items: the per-CPU caches
	# follow by difference), the VM's free and wired pages, and the
	# system's context switches, interrupts, soft interrupts and syscalls.
	if [ -n "${PB_CTR:-}" ]; then
		if [ "$WCPU" = any ]; then	# compare.sh OOB=yes: W is known only after the run, so every worker's passes and wakeups
			_names="kern.cp_times $(sysctl -N kern.kwq.net.cpu 2>/dev/null | grep -E '\.(passes|wakeups)$' | tr '\n' ' ')"
		else
			_w="kern.kwq.net.cpu.$WCPU"
			_names="kern.cp_times $_w.wakeups $_w.passes $_w.ticks $_w.busy_ns $_w.idle_ns $_w.nnew $_w.nactive $_w.yields $_w.switched"
			_names="$_names $(sysctl -N kern.kwq.net.queue 2>/dev/null | grep -E "\.cpu\.$WCPU\.(items|doorbells|doorbells_skipped)\$" | tr '\n' ' ')"
		fi
		for _z in mbuf_jumbo_page mbuf; do
			for _n in stats.allocs stats.frees stats.current domain.0.nitems domain.0.imin domain.0.imax \
			    domain.0.wss keg.domain.0.pages keg.domain.0.free_items keg.domain.0.free_slabs; do
				_names="$_names vm.uma.$_z.$_n"
			done
		done
		_names="$_names vm.stats.vm.v_free_count vm.stats.vm.v_wire_count"
		_names="$_names vm.stats.sys.v_swtch vm.stats.sys.v_intr vm.stats.sys.v_soft vm.stats.sys.v_syscall"
		# PB_CTR_EXTRA="sysctl ...": more counters on the same grid
		_names="$_names ${PB_CTR_EXTRA:-}"
		# PB_NETX=N: every Nth sample also netstat -x in both jails
		if [ -n "${PB_NETX:-}" ]; then
			export CTR_CMD="jexec $JC netstat -xn -p tcp; jexec $JS netstat -xn -p tcp" CTR_EVERY="$PB_NETX"
		fi
		# shellcheck disable=SC2086
		"$(dirname "$0")/ctrsample" "$PB_CTR" "$((SECS + 1))" $_names > "$DIR/ctr.txt" 2>&1 &
		echo $! > "$DIR/ctr.pid"
	fi
	# After the iperf3 threads exist: a process-mode PMC does not follow a
	# thread created after the attach (the server's stream thread).
	sleep 1
	# PB_SAMPLE="pmcstat sampling options" (e.g. "-n 2000 -P EVENT") samples
	# the client process with call chains into samp_c.log, for pmcstat -R
	# ... -G (where the DRAM fills happen); the counters drop to cycles and
	# instructions so that the rows fit the six counters, and the client
	# is not counted (hwpmc refuses a second session on one process).
	if [ -n "${PB_SAMPLE:-}" ]; then
		EV="ls_not_halted_cyc ex_ret_instr"
		# shellcheck disable=SC2086
		pmcstat $PB_SAMPLE -t "$CPID" -O "$DIR/samp_c.log" > "$DIR/samp_c.err" 2>&1 &
		echo $! > "$DIR/samp_c.pid"
	fi
	_pa=""; for _e in $EV; do _pa="$_pa -p $_e"; done
	# PB_NOPROC=yes attaches nothing to the generators: process-mode PMCs
	# run hwpmc's switch hook inside sched_switch()'s blocked-lock window,
	# which is what a waker of the receiver spins on (WAKE.md S12.2).
	if [ "${PB_NOPROC:-no}" != yes ]; then
		# shellcheck disable=SC2086
		[ -z "${PB_SAMPLE:-}" ] &&	# hwpmc takes one pmcstat per process
		    { pmcstat -C -w 1 $_pa -t "$CPID" > "$DIR/pmc_c" 2>&1 & echo $! > "$DIR/pmc_c.pid"; }
		# shellcheck disable=SC2086
		pmcstat -C -w 1 $_pa -t "$SPID" > "$DIR/pmc_s" 2>&1 & echo $! > "$DIR/pmc_s.pid"
	fi
	[ "${PB_DLIGHT:-no}" = yes ] && dlight
	[ "${PB_DFREE:-no}" = yes ] && dfree
	[ "${PB_DREUSE:-no}" = yes ] && dreuse
	# PB_SYSPMC="pmcstat system-mode options" (e.g. "-c 4,6 -s EVENT ...")
	# counts per CPU, which hooks nothing in the context switch path
	if [ -n "${PB_SYSPMC:-}" ]; then
		# shellcheck disable=SC2086
		pmcstat -C -w "${PB_SYSPMC_W:-1}" $PB_SYSPMC > "$DIR/syspmc" 2>&1 &
		echo $! > "$DIR/syspmc.pid"
	fi
	# PB_SYSSAMP="CPU..." samples cycles in system mode on each listed CPU
	# with call chains (one pmcstat and log per CPU, so the profiles stay
	# apart), every PB_SYSSAMP_N (1000000) events of PB_SYSSAMP_EV (AMD's
	# ls_not_halted_cyc; cpu_cycles on arm64); no switch hooks.
	for _c in ${PB_SYSSAMP:-}; do
		pmcstat -c "$_c" -n "${PB_SYSSAMP_N:-1000000}" -S "${PB_SYSSAMP_EV:-ls_not_halted_cyc}" -O "$DIR/samp_cpu$_c.log" > "$DIR/samp_cpu$_c.err" 2>&1 &
		echo $! > "$DIR/samp_cpu$_c.pid"
	done
	# PB_SAMPLE_W="pmcstat options" samples W's CPU in system mode
	if [ -n "${PB_SAMPLE_W:-}" ]; then
		# shellcheck disable=SC2086
		pmcstat -c "$WCPU" $PB_SAMPLE_W -O "$DIR/samp_w.log" > "$DIR/samp_w.err" 2>&1 &
		echo $! > "$DIR/samp_w.pid"
	fi
	[ "$ROTATE" = yes ] && case $((REP % 3)) in
	2) lockstat -C -D 30 sleep $((SECS - 3)) > "$DIR/lockstat" 2>&1 & echo $! > "$DIR/probe.pid" ;;
	0) profile ;;
	esac
	( sleep $((SECS / 4)); mid q1; sleep $((SECS / 2)); mid q3 ) > /dev/null 2>&1 &
	echo $! > "$DIR/mid.pid"
	;;
stop)
	for _p in mid probe thr ctr; do [ -f "$DIR/$_p.pid" ] && pwait -t 20 "$(cat "$DIR/$_p.pid")" 2>/dev/null; done
	# A row after the server's stream thread has exited, which is when its
	# count is last refreshed if it never switched out (the slow mode)
	sleep 2
	for _p in pmc_c pmc_s samp_c samp_w syspmc $(cd "$DIR" && ls samp_cpu*.pid 2>/dev/null | sed 's/\.pid$//'); do
		_q=$(cat "$DIR/$_p.pid" 2>/dev/null) || continue
		kill -INT "$_q" 2>/dev/null; pwait -t 5 "$_q" 2>/dev/null || kill -KILL "$_q" 2>/dev/null
	done
	snap 1
	[ "${PB_STEAL_AB:-no}" = yes ] && sysctl -q kern.sched.ule.steal_idle="${PB_STEAL_ORIG:-1}" > /dev/null
	;;
esac
exit 0
