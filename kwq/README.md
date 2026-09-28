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
                  since DRR (S15), pseudo-interface batching (S16),
                  memory layout (S17); revision log last
    SCHED.md      worker scheduler specification (prior art survey,
                  algorithm, invariants, bounds, test plan); normative
                  for P1
    GLOSSARY.md   canonical terms (FreeBSD's where they exist), aliases
                  from other systems, and the words deliberately avoided
    PLAN.txt      phased implementation plan with exit criteria
    kwq/          kwq.ko - the service (kwq.h public KPI, kwq.c,
                  kwq_worker.c; P2 adds kwq_sdt.c, kwq_ddb.c, kwq.d)
    sim/          userspace simulator of the scheduler: links
                  kwq/kwq_sched.c against mock clocks; `make -C sim test`
                  runs SCHED.md's invariants, bounds, corner cases and
                  wrap-around runs in about a second (13 scenarios, 100
                  random seeds); `./kwqsim <scenario> -s seed -v` for one
    kwq_test/     kwq_test.ko - synthetic clients driven by sysctl
    tests/        kwqvm.sh, which builds and runs the disposable
                  15.1/amd64 GENERIC-DEBUG bhyve guest the module tests
                  need (doas tests/kwqvm.sh setup; start; then mods and
                  ssh as your user); the ATF harness (Kyuafile,
                  kwq_lib.sh, t_*.sh) is P3
    examples/     (P8) compilable usage examples (deferred.c, scatter.c,
                  pair_client.c)
    man/          (P8) kwq.9, dtrace_kwq.4

Background research is in the parent directory: POOLS.md (cross-OS and
literature survey) and DISPATCH.md (libdispatch study).  The driver this
grew out of, and its measurements, are in ../if_pair.c and ../NOTES.md.

Build: make (both modules, against /usr/src/sys); for the test guest's
kernel, tests/kwqvm.sh mods.  Load: kldload ./kwq/kwq.ko
./kwq_test/kwq_test.ko; drive kwq_test through sysctl kern.kwq_test
(scenario, items, reps, run, result_*).  Status: P0, P1a and P1b done (measured in the bhyve guest on both kernels and on a07, SCHED.md S10.2-S10.3); P2 (observability) is next.  tests/run_p1b.sh runs the guest scenarios (MODDIR=/root/kwq-generic after `kwqvm.sh mods generic` for the stock GENERIC kernel, booted with `nextboot -k kernel`).
