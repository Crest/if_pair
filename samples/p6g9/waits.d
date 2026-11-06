/*
 * waits.d - how long a 1 MiB write's data waits in GELI and md(4), stock
 * GELI (module geom_eli) or kwq-geli (geom_kwqeli, preloaded), in the bulk
 * phases of geli_bench scale at SCALE_NS="32 128" (the 1st and 3rd iolat
 * run), wall time, us: plain_wait from g_eli_start() to the first
 * g_eli_crypto_run() (the plaintext waits for GELI's copy); span from
 * there to g_io_request() of the encrypted child (the copy, the
 * encryption and, under kwq-geli, the gaps between a cut bio's pieces);
 * cipher_wait from there to md's mdstart_swap() (the ciphertext waits for
 * md's copy).  Locality: where a cut bio's later pieces run against its
 * first, and where md copies against where the child was issued (lab's
 * SMT siblings are 2k and 2k+1).  $1: the module.  (2026-11-05: the first version keyed the
 * first piece on `timestamp' being equal across clauses, which it is not.)
 */
#pragma D option quiet
#pragma D option dynvarsize=64m
int nexec;
string phase;
uint64_t arr[uint64_t], beg[uint64_t], rdy[uint64_t];
int bcpu[uint64_t], rcpu[uint64_t];
BEGIN { phase = "setup"; }
proc:::exec-success /execname == "iolat"/ {
	nexec++; phase = nexec == 1 ? "32 bulk" : nexec == 3 ? "128 bulk" : "other";
}
proc:::exit /execname == "iolat"/ { phase = "between"; }
fbt:$1:g_eli_start:entry /(phase == "32 bulk" || phase == "128 bulk") && ((struct bio *)arg0)->bio_cmd == 2/ {
	arr[arg0] = timestamp;
}
fbt:$1:g_eli_crypto_run:entry /arr[arg1] != 0 && beg[arg1] == 0/ {
	this->t = timestamp;
	beg[arg1] = this->t; bcpu[arg1] = cpu + 1;
	this->d = this->t > arr[arg1] ? this->t - arr[arg1] : 0;
	@plain_wait_us[phase] = quantize(this->d / 1000); @plain_avg[phase] = avg(this->d / 1000);
	@bios[phase] = count();
}
fbt:$1:g_eli_crypto_run:entry /arr[arg1] != 0/ { @pieces[phase] = count(); }
fbt:$1:g_eli_crypto_run:entry /arr[arg1] != 0 && beg[arg1] != this->t && bcpu[arg1] != 0/ {
	@later_piece[phase, bcpu[arg1] - 1 == cpu ? "same CPU" : ((bcpu[arg1] - 1) ^ 1) == cpu ? "SMT sibling" : "other core"] = count();
}
fbt::g_io_request:entry /beg[(uint64_t)args[0]->bio_parent] != 0/ {
	this->p = (uint64_t)args[0]->bio_parent; this->t = timestamp;
	this->d = this->t > beg[this->p] ? this->t - beg[this->p] : 0;
	@span_us[phase] = quantize(this->d / 1000); @span_avg[phase] = avg(this->d / 1000);
	rdy[this->p] = this->t; rcpu[this->p] = cpu + 1; arr[this->p] = 0; beg[this->p] = 0; bcpu[this->p] = 0;
}
fbt::mdstart_swap:entry /rdy[(uint64_t)args[1]->bio_parent] != 0/ {
	this->p = (uint64_t)args[1]->bio_parent; this->t = timestamp;
	this->d = this->t > rdy[this->p] ? this->t - rdy[this->p] : 0;
	@cipher_wait_us[phase] = quantize(this->d / 1000); @cipher_avg[phase] = avg(this->d / 1000);
	@md_cpu[phase, rcpu[this->p] - 1 == cpu ? "same CPU" : ((rcpu[this->p] - 1) ^ 1) == cpu ? "SMT sibling" : "other core"] = count();
	rdy[this->p] = 0; rcpu[this->p] = 0;
}
END {
	printa("bios %s %@d\n", @bios); printa("pieces %s %@d\n", @pieces);
	printa("plain_wait_avg_us %s %@d\n", @plain_avg); printa("span_avg_us %s %@d\n", @span_avg);
	printa("cipher_wait_avg_us %s %@d\n", @cipher_avg);
	printa("later_piece %s %s %@d\n", @later_piece); printa("md_cpu %s %s %@d\n", @md_cpu);
	printf("\n== plain_wait_us\n"); printa(@plain_wait_us);
	printf("\n== span_us\n"); printa(@span_us);
	printf("\n== cipher_wait_us\n"); printa(@cipher_wait_us);
}
