# kwq - kernel work queues for FreeBSD (out-of-tree module)

Everything kwq lives under this directory; nothing here is built by the
if_pair Makefile in the parent directory.

    KWQ.md        design: threading model, API contracts (including
                  the kwq_notify signal primitive), lifecycle and
                  locking, integration, accounting goals (the
                  scheduler itself is in SCHED.md), callback rules,
                  locks-vs-lock-free decision, DTrace and sysctl
                  reference, gap analysis (S11), module-first rationale
                  and trade-offs (S12), GELI (S14), scheduler literature
                  since DRR (S15), pseudo-interface batching and the
                  opt-in wake regimes (S16), memory layout (S17);
                  revision log last
    SCHED.md      worker scheduler specification (prior art survey,
                  algorithm, invariants, bounds, test plan); normative
                  for P1
    WAKE.md       wake moderation (PLAN P4e): literature, FreeBSD prior
                  art, candidates, and the batteries on three machines
                  behind the tick regime, the idle-class wait and the
                  urgent exemption (S7-S12)
    BULK.md       the BULK class (PLAN P6): what was built, the crypto
                  client and kwq-geli against GELI's model, the faults
                  found (two in the share limit), BULK placement's
                  results and gates (P6g, S8), what is open
    SPEC-p6g.md   the plan of P6g, BULK placement: the gossip scheme
                  and P8, the simulator, the kernel steps, the criteria
                  and decisions D1-D15
    OBSERVABILITY.md
                  tunables, counters, DTrace probes, DDB and the test
                  runner, with the commands to type
    GLOSSARY.md   canonical terms (FreeBSD's where they exist), aliases
                  from other systems, and the words deliberately avoided
    PLAN.txt      phased implementation plan with exit criteria
    docs/         technical documentation for client developers and
                  reviewers: overview.md (the model, five rules and a
                  tutorial), cheatsheet.md (patterns, warnings,
                  anti-patterns), reference.md (every function, flag,
                  error, sysctl and probe), whitepaper.md (design
                  rationale, prior art and measurements); start at
                  docs/README.md.  KWQ.md and SCHED.md stay normative
    kwq/          kwq.ko - the service (kwq.h public KPI, kwq.c,
                  kwq_worker.c, kwq_sched.c, kwq_clock.c, kwq_sdt.c,
                  kwq_ddb.c, kwq.d; BULK placement in kwq_shard.c, P8,
                  and kwq_gossip.c with kwq_place.c, the gossip scheme,
                  SCHED.md S17)
    sim/          userspace simulator of the scheduler: links
                  kwq/kwq_sched.c against mock clocks; `make -C sim test`
                  runs SCHED.md's invariants, bounds, corner cases and
                  wrap-around runs in under a minute (20 scenarios, 100
                  random seeds); `./kwqsim <scenario> -s seed -v` for one
    sim/place/    placesim, the simulator of BULK placement (SPEC-p6g.md
                  S3): BULK workers, producers, other threads and the
                  cache lines the policies touch, policy P5 running the
                  kernel's kwq_place.c; matrix.py runs the scenario
                  matrix into out/ (gitignored), summarize.py judges the
                  criteria, report.py writes the G1 page's datasets
    kwq_test/     kwq_test.ko - synthetic clients driven by sysctl
    kwq_crypto/   kwq_crypto.ko - the dummy BULK client (P6): AES-XTS
                  through crypto(9) the way GELI does, on BULK queues or
                  on GELI's own thread model for comparison
    tests/        kwqvm.sh, which builds and runs the disposable
                  15.1/amd64 GENERIC-DEBUG bhyve guest the module tests
                  need (doas tests/kwqvm.sh setup; start; then mods and
                  ssh as your user); the ATF suite (Kyuafile,
                  kwq_test.sh, kwq_crypto_test.sh; run under kyua,
                  OBSERVABILITY.md S9); run_p1b.sh, the performance
                  runner; crypto_bench.sh, kwq_crypto against its
                  thread baseline
    examples/     (PLAN P8) compilable usage examples (deferred.c, scatter.c,
                  pair_client.c)
    man/          (PLAN P8) kwq.9, dtrace_kwq.4

The BULK class's proof client, GELI ported to BULK queues and tested on
md(4), is ../kwq-geli (PLAN.txt P6).

Background research is in the parent directory: POOLS.md (cross-OS and
literature survey) and DISPATCH.md (libdispatch study).  The driver this
grew out of, and its measurements, are in ../if_pair.c and ../NOTES.md.

Build: make (the three modules, against /usr/src/sys); for the test guest's
kernel, tests/kwqvm.sh mods.  Load: kldload ./kwq/kwq.ko
./kwq_test/kwq_test.ko; drive kwq_test through sysctl kern.kwq_test
(scenario, items, reps, run, result_*).  Status: P0 through P4 done: if_pair delivers through kwq, measured against the frozen taskqueue driver (../extras/if_pair_tq) on the guest and on a07 (../NOTES.md, PLAN.txt P4); P4b closed, P4c done; P4d's pass-level share limit (`cap_prio` 224) and P4e's wake moderation (the grace rule in service, the opt-in tick regime with `tick_urgent_us`, the idle-class wait, `kwq_enqueue_urgent()` and if_pair's `net.link.pair.urgent_bytes`) are merged and measured on the guest, lab and a07 (WAKE.md S11-S12), with only the NIC-fed flow still open (PLAN.txt P4d, P4e); P5 (epair) is next.  Merged from branch kwq-bulk on 2026-11-10: P6, the BULK class with stealing, kwq_scatter() and kwq-geli (BULK.md), and P6g, BULK placement, P8 with the chain the default on a machine of one NUMA domain (SCHED.md S17, BULK.md S8).  The ATF suite: `kyua -v test_suites.kwq.moddir=<modules> test -k tests/Kyuafile` as root on a machine with 4+ CPUs (OBSERVABILITY.md S9).  Tunables, sysctl counters, DTrace probes, DDB and the test runner are documented with examples in OBSERVABILITY.md.  tests/run_p1b.sh runs the scenarios: in the guest by default (MODDIR=/root/kwq-generic after `kwqvm.sh mods generic` for the stock GENERIC kernel, booted with `nextboot -k kernel`), on a07 with `KWQ_SSH="ssh a07 doas -n" MODDIR=/home/crest/if_pair/kwq` against an in-tree build there.  `make` in this directory builds the three modules with probes and CTF; `make install` and `make uninstall` handle /boot/modules and the translator.
