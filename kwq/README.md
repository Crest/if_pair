# kwq - kernel work queues for FreeBSD (out-of-tree module)

Everything kwq lives under this directory; nothing here is built by the
if_pair Makefile in the parent directory.

    KWQ.md        design: threading model, API contracts, lifecycle and
                  locking, integration, accounting, callback rules,
                  locks-vs-lock-free decision, DTrace, gap analysis,
                  module-first rationale
    PLAN.txt      phased implementation plan with exit criteria
    kwq/          kwq.ko - the service (kwq.h public KPI, kwq.c,
                  kwq_worker.c, kwq_sdt.c, kwq_ddb.c, kwq.d translator)
    kwq_test/     kwq_test.ko - synthetic clients driven by sysctl
    tests/        ATF harness (Kyuafile, kwq_lib.sh, t_*.sh)
    examples/     compilable usage examples (deferred.c, scatter.c,
                  pair_client.c)
    man/          kwq.9, dtrace_kwq.4

Background research is in the parent directory: POOLS.md (cross-OS and
literature survey) and DISPATCH.md (libdispatch study).  The driver this
grew out of, and its measurements, are in ../if_pair.c and ../NOTES.md.

Build (once P0 lands): make -C kwq && make -C kwq_test; load with
kldload ./kwq/kwq.ko.  Tests: kyua test -k tests/Kyuafile.
