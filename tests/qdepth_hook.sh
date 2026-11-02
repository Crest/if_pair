#!/bin/sh
# qdepth_hook.sh - a TPUT1_HOOK for compare.sh OOB=yes (crest, 2026-11-01):
# if_pair's per-CPU queue depth in the steady middle of one TCP flow, and a
# sweep of the per-queue item limit.  Wraps perbyte_hook.sh, same arguments:
#
#   qdepth_hook.sh pre|start|stop DIR CPID SPID SECS JC JS WCPU REP
#
# pre    a stage labelled Q<N> (DIR is OUT/hook.<stage>.<rep>) sets the
#        limit of every pair queue to N (kern.kwq.net.queue.<q>.limit,
#        writable at run time; the pair is created per stage and
#        repetition, so the setting dies with it); DIR/qlimit records it
# start  in the background: at QD_T0 seconds (10) finds the flow's worker,
#        the CPU whose passes grew most over 0.5 s, zeroes maxdepth (reset),
#        samples the depth of both pair queues on that CPU every QD_US (1000)
#        us for QD_SECS (10) s with ctrsample (DIR/qdepth.txt), then dumps
#        both queues (DIR/qdepth.end: maxdepth, rejected, items, passes)
# stop   waits for that job, then perbyte_hook.sh stop
#
# The data direction (client on pairNb to server on pairNa) is queue
# pairNa; the server's ACKs travel on pairNb's queue, on the same CPU
# through the inherited flowid.
PH=$1; DIR=$2
HOOK=$(dirname "$0")/perbyte_hook.sh
queues() { sysctl -N kern.kwq.net.queue 2>/dev/null | sed -n 's/^kern\.kwq\.net\.queue\.\([^.]*\)\.limit$/\1/p'; }
passes() { sysctl kern.kwq.net.cpu 2>/dev/null | awk -F'[.:]' '$6 == "passes" { print $5, $NF }'; }
case $PH in
pre)
	sh "$HOOK" "$@"
	_s=$(basename "$DIR"); _s=${_s#hook.}; _s=${_s%.*}
	case $_s in
	Q[0-9]*) for _q in $(queues); do sysctl -q "kern.kwq.net.queue.$_q.limit=${_s#Q}" > /dev/null; done ;;
	esac
	for _q in $(queues); do echo "$_q $(sysctl -n "kern.kwq.net.queue.$_q.limit")"; done > "$DIR/qlimit"
	;;
start)
	sh "$HOOK" "$@"
	(
		sleep "${QD_T0:-10}"
		_a=$(passes); sleep 0.5; _b=$(passes)
		_w=$(printf '%s\n%s\n' "$_a" "$_b" | awk '{ d[$1] = $2 - d[$1] } END { for (c in d) if (d[c] > m) { m = d[c]; w = c } if (m > 0) print w }')
		echo "${_w:--}" > "$DIR/qdepth.w"
		[ -n "$_w" ] || exit 0
		for _q in $(queues); do sysctl -q "kern.kwq.net.queue.$_q.reset=1" > /dev/null; done
		_n=""; for _q in $(queues); do _n="$_n kern.kwq.net.queue.$_q.cpu.$_w.depth"; done
		# shellcheck disable=SC2086
		"$(dirname "$0")/ctrsample" "${QD_US:-1000}" "${QD_SECS:-10}" $_n > "$DIR/qdepth.txt" 2>&1
		for _q in $(queues); do sysctl "kern.kwq.net.queue.$_q"; done > "$DIR/qdepth.end" 2>&1
	) > /dev/null 2>&1 &
	echo $! > "$DIR/qd.pid"
	;;
stop)
	[ -f "$DIR/qd.pid" ] && pwait -t 30 "$(cat "$DIR/qd.pid")" 2>/dev/null
	sh "$HOOK" "$@"
	;;
esac
exit 0
