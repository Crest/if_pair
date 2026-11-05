/*
 * where.d - where lab's CPU goes per MiB written: geli_bench scale at
 * SCALE_NS="16 32 128", one stage, REPS=1.  Phases are the n-th iolat run
 * (1 16 bulk, 2 16 small, 3 32 bulk, 4 32 small, 5 128 bulk, 6 128
 * small); per phase a 997 Hz profile of every CPU by thread kind and
 * kernel function, the phase's wall time, and kwq's passes, requeues and
 * share-limit events.  Needs kwq.d (-L) for the kwq probes.
 */
#pragma D option quiet
#pragma D option aggsize=32m
#pragma D option dynvarsize=16m
int nexec;
string phase;
BEGIN { phase = "setup"; }
proc:::exec-success /execname == "iolat"/ {
	nexec++;
	phase = nexec == 1 ? "16 bulk" : nexec == 2 ? "16 small" : nexec == 3 ? "32 bulk" :
	    nexec == 4 ? "32 small" : nexec == 5 ? "128 bulk" : nexec == 6 ? "128 small" : "later";
	t0 = timestamp;
}
proc:::exit /execname == "iolat" && phase != "between"/ { @dur_ms[phase] = sum((timestamp - t0) / 1000000); phase = "between"; }
profile-997 /phase != "setup" && phase != "between"/ {
	this->k = execname == "idle" ? "idle" :
	    execname == "iolat" ? (arg0 != 0 ? "iolat kernel" : "iolat user") :
	    execname == "intr" ? "intr" :
	    execname == "geom" ? "geom" :
	    substr(execname, 0, 6) == "g_eli[" ? "geli worker" :
	    substr(execname, 0, 2) == "md" && execname != "mdconfig" ? "md" :
	    execname == "kernel" && substr(stringof(curthread->td_name), 0, 8) == "kwq_bulk" ? "kwq bulk" :
	    execname == "kernel" ? "kernel other" : arg0 != 0 ? "other kernel" : "other user";
	@cls[phase, this->k] = count();
}
profile-997 /phase != "setup" && phase != "between" && arg0 != 0 && execname != "idle"/ {
	@fn[phase, this->k, func(arg0)] = count();
}
kwq:::pass-end /phase != "setup" && phase != "between"/ { @passes[phase] = count(); @requeued[phase] = sum(arg4); @items[phase] = sum(arg2); }
kwq:::cap /phase != "setup" && phase != "between"/ { @cap[phase, arg2] = count(); }
END {
	printa("dur_ms\t%s\t%@d\n", @dur_ms);
	printa("cls\t%s\t%s\t%@d\n", @cls);
	printa("passes\t%s\t%@d\n", @passes); printa("items\t%s\t%@d\n", @items); printa("requeued\t%s\t%@d\n", @requeued);
	printa("cap\t%s\t%d\t%@d\n", @cap);
	printa("fn\t%s\t%s\t%a\t%@d\n", @fn);
}
