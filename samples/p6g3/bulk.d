/*
 * bulk.d - geli_bench scale at SCALE_NS="16 128", one stage, REPS=1: per
 * phase (the n-th iolat start: 16 bulk, 16 small, 128 bulk, 128 small) the
 * kwq passes, steals, share limit and BULK worker switches, and a kernel
 * profile of the two bulk phases.  Needs kwq.d (-L) for the kwq probes.
 */
#pragma D option quiet
#pragma D option aggsize=16m
#pragma D option dynvarsize=16m
int nexec;
string phase;
BEGIN { phase = "setup"; }
proc:::exec-success /execname == "iolat"/ {
	nexec++;
	phase = nexec == 1 ? "16 bulk" : nexec == 2 ? "16 small" : nexec == 3 ? "128 bulk" : nexec == 4 ? "128 small" : "later";
	t0 = timestamp;
}
proc:::exit /execname == "iolat"/ { @dur_ms[phase] = sum((timestamp - t0) / 1000000); phase = "between"; }
kwq:::pass-end /phase != "setup" && phase != "between"/ {
	@passes[phase] = count(); @items[phase] = sum(arg2); @pass_us[phase] = sum(arg3 / 1000); @requeued[phase] = sum(arg4);
}
kwq:::steal /phase != "between"/ { @steal_items[phase, arg4] = sum(arg3); @steal_batches[phase, arg4] = count(); }
kwq:::steal-miss /phase != "between"/ { @misses[phase, arg1, arg3] = count(); }
kwq:::steal-wake /phase != "between"/ { @swakes[phase, arg2, arg1 >= 0 ? 1 : 0] = count(); }
kwq:::cap /phase != "between"/ { @cap[phase, arg2] = count(); }
kwq:::yield /phase != "between"/ { @yields[phase, arg2] = count(); }
kwq:::overrun /phase != "between"/ { @overruns[phase] = count(); }
kwq:::budget-hit /phase != "between"/ { @budget_hits[phase] = count(); }
kwq:::doorbell /phase != "between"/ { @doorbells[phase, arg2] = count(); }
sched:::off-cpu /phase != "between" && strstr(stringof(curthread->td_name), "kwq_bulk") != NULL/ { @off_kwq[phase] = count(); }
sched:::off-cpu /phase != "between" && strstr(execname, "g_eli") != NULL/ { @off_geli[phase] = count(); }
profile-997 /arg0 && phase == "16 bulk"/ { @p16[func(arg0)] = count(); @all16 = count(); }
profile-997 /arg0 && phase == "128 bulk"/ { @p128[func(arg0)] = count(); @all128 = count(); }
END {
	printa("dur_ms %s %@d\n", @dur_ms);
	printa("passes %s %@d\n", @passes); printa("items %s %@d\n", @items);
	printa("pass_us %s %@d\n", @pass_us); printa("requeued %s %@d\n", @requeued);
	printa("steal_items %s how=%d %@d\n", @steal_items); printa("steal_batches %s how=%d %@d\n", @steal_batches);
	printa("misses %s how=%d why=%d %@d\n", @misses); printa("swakes %s why=%d woken=%d %@d\n", @swakes);
	printa("cap %s event=%d %@d\n", @cap); printa("yields %s why=%d %@d\n", @yields);
	printa("overruns %s %@d\n", @overruns); printa("budget_hits %s %@d\n", @budget_hits);
	printa("doorbells %s state=%d %@d\n", @doorbells);
	printa("off_kwq %s %@d\n", @off_kwq); printa("off_geli %s %@d\n", @off_geli);
	printa("kernel samples, 16 bulk: %@d\n", @all16); printa("kernel samples, 128 bulk: %@d\n", @all128);
	trunc(@p16, 30); trunc(@p128, 30);
	printa("p16 %a %@d\n", @p16); printa("p128 %a %@d\n", @p128);
}
