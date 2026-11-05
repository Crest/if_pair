/* probes.d: every BULK placement probe aggregated; needs kwq.d (-L). */
kwq:::steal { @steal[arg4, args[0]->kwq_name] = sum(arg3); @left[arg4] = quantize(arg5); }
kwq:::steal-wake { @wake[arg2, arg1 >= 0 ? 1 : 0, arg3] = count(); }
kwq:::steal-miss { @miss[arg1, arg3] = count(); }
kwq:::donor { @donor[arg1, arg2] = count(); }
kwq:::place { @place[arg3, arg4] = count(); }
kwq:::steal /arg4 == KWQ_STEAL_CHAIN/ { @chained = count(); }
END {
	printa("steal how=%d %s items=%@d\n", @steal);
	printa("steal how=%d, items left on the victim:%@d\n", @left);
	printa("steal-wake why=%d woken=%d mailbox=%d n=%@d\n", @wake);
	printa("steal-miss how=%d why=%d n=%@d\n", @miss);
	printa("donor on=%d starved=%d n=%@d\n", @donor);
	printa("place step=%d spill=%d n=%@d\n", @place);
	printa("chained batches (KWQ_STEAL_CHAIN from kwq.d): %@d\n", @chained);
}
