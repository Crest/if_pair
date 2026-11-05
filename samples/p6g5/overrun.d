/*
 * overrun.d - kwq-geli's BULK passes over their budget (SCHED.md S4.3):
 * geli_bench scale at SCALE_NS="16 128", the kwqshard stage, REPS=1, in
 * the two bulk phases.  Per overrun: the excess, the budget, the pass in
 * kwq's CPU time against dtrace's vtimestamp and wall time, the items and
 * the longest stretch between kwq_budget_left() calls; stacks of the
 * worker when it has gone 300 us of CPU time without one; DRR parks, the
 * deficit at the park and the wall time until the queue's next pass.
 * Needs kwq.d (-L).
 */
#pragma D option quiet
#pragma D option aggsize=16m
#pragma D option dynvarsize=32m
int nexec;
string phase;
uint64_t parked[int64_t, int64_t];
BEGIN { phase = "setup"; }
proc:::exec-success /execname == "iolat"/ {
	nexec++;
	phase = nexec == 1 ? "16 bulk" : nexec == 2 ? "16 small" : nexec == 3 ? "128 bulk" : nexec == 4 ? "128 small" : "later";
}
proc:::exit /execname == "iolat"/ { phase = "between"; }

kwq:::park /phase == "16 bulk" || phase == "128 bulk"/ {
	@parks[phase] = count();
	@park_def_us[phase] = quantize(-(int64_t)arg2 / 1000);
	parked[arg0, arg1] = parked[arg0, arg1] != 0 ? parked[arg0, arg1] : timestamp;
}
kwq:::pass-start /parked[arg0, arg1] != 0/ {
	@park_wait_us[phase] = quantize((timestamp - parked[arg0, arg1]) / 1000);
	parked[arg0, arg1] = 0;
}
kwq:::pass-start /phase == "16 bulk" || phase == "128 bulk"/ {
	self->in = 1; self->dt = 0; self->calls = 0; self->gv = 0; self->gw = 0;
	self->v0 = self->vl = vtimestamp; self->w0 = self->wl = timestamp;
}
fbt:kwq:kwq_budget_left:entry /self->in/ {
	this->gv = vtimestamp - self->vl; this->gw = timestamp - self->wl;
	self->gv = this->gv > self->gv ? this->gv : self->gv;
	self->gw = this->gw > self->gw ? this->gw : self->gw;
	@gap_v_us[phase] = quantize(this->gv / 1000);
	self->vl = vtimestamp; self->wl = timestamp; self->calls++;
}
fbt:kwq:kwq_budget_left:entry /self->in && this->gv > 300000/ { @gapsite[phase, stack(4)] = count(); }
profile-4999 /self->in && vtimestamp - self->vl > 300000/ { @slow[phase, stack(14)] = count(); @slow_n[phase] = count(); }
kwq:::pass-end /self->in/ {
	this->gv = vtimestamp - self->vl; this->gw = timestamp - self->wl;
	self->gv = this->gv > self->gv ? this->gv : self->gv;
	self->gw = this->gw > self->gw ? this->gw : self->gw;
	@tail_v_us[phase] = quantize(this->gv / 1000);
	self->dt = arg3; self->n = arg2; self->rq = arg4;
	self->vdt = vtimestamp - self->v0; self->wdt = timestamp - self->w0;
	self->in = 0;
	@passes[phase] = count(); @pass_us[phase] = quantize(arg3 / 1000);
	@pass_gap_v_us[phase] = quantize(self->gv / 1000);
}
kwq:::overrun /self->dt != 0/ {
	@ov[phase] = count();
	@ov_over_us[phase] = quantize(arg2 / 1000);
	@ov_budget_us[phase] = quantize((int64_t)(self->dt - arg2) / 1000);
	@ov_dt_us[phase] = quantize(self->dt / 1000);
	@ov_vdt_us[phase] = quantize(self->vdt / 1000);
	@ov_wdt_us[phase] = quantize(self->wdt / 1000);
	@ov_n[phase] = quantize(self->n);
	@ov_rq[phase, self->rq > 0 ? 1 : 0] = count();
	@ov_gap_v_us[phase] = quantize(self->gv / 1000);
	@ov_gap_w_us[phase] = quantize(self->gw / 1000);
	@ov_calls[phase] = quantize(self->calls);
	self->dt = 0;
}
/* Counts every overrun: the clause above zeroed self->dt (the run of 2026-11-05; read 0). */
kwq:::overrun /self->dt == 0 && phase != "between"/ { @ov_unmatched[phase] = count(); }
END {
	printa("passes %s %@d\n", @passes); printa("ov %s %@d\n", @ov);
	printa("ov_unmatched %s %@d\n", @ov_unmatched); printa("ov_rq %s requeued=%d %@d\n", @ov_rq);
	printa("parks %s %@d\n", @parks); printa("slow_n %s %@d\n", @slow_n);
	printf("\n== ov_over_us\n"); printa(@ov_over_us);
	printf("\n== ov_budget_us\n"); printa(@ov_budget_us);
	printf("\n== ov_dt_us (kwq CPU time)\n"); printa(@ov_dt_us);
	printf("\n== ov_vdt_us (vtimestamp)\n"); printa(@ov_vdt_us);
	printf("\n== ov_wdt_us (wall)\n"); printa(@ov_wdt_us);
	printf("\n== ov_n\n"); printa(@ov_n);
	printf("\n== ov_calls\n"); printa(@ov_calls);
	printf("\n== ov_gap_v_us\n"); printa(@ov_gap_v_us);
	printf("\n== ov_gap_w_us\n"); printa(@ov_gap_w_us);
	printf("\n== pass_us\n"); printa(@pass_us);
	printf("\n== pass_gap_v_us\n"); printa(@pass_gap_v_us);
	printf("\n== gap_v_us\n"); printa(@gap_v_us);
	printf("\n== tail_v_us\n"); printa(@tail_v_us);
	printf("\n== park_def_us\n"); printa(@park_def_us);
	printf("\n== park_wait_us\n"); printa(@park_wait_us);
	printf("\n== gapsite\n"); printa(@gapsite);
	trunc(@slow, 25); printf("\n== slow\n"); printa(@slow);
}
