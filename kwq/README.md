# kwq - kernel work queues for FreeBSD (out-of-tree module)

Everything kwq lives under this directory; nothing here is built by the
if_pair Makefile in the parent directory.

    KWQ.md        design: threading model, API contracts (including
                  the kwq_notify signal primitive), lifecycle and
                  locking, integration, accounting (DRR with the
                  fq_codel new-list rule), callback rules,
                  locks-vs-lock-free decision, DTrace and sysctl
                  reference, gap analysis (S11), module-first rationale
                  and trade-offs (S12), GELI (S14), scheduler literature
                  since DRR (S15), pseudo-interface batching (S16),
                  memory layout (S17); revision log last
    GLOSSARY.md   canonical terms (FreeBSD's where they exist), aliases
                  from other systems, and the words deliberately avoided
    PLAN.txt      phased implementation plan with exit criteria
    kwq/          kwq.ko - the service (kwq.h public KPI, kwq.c,
                  kwq_worker.c, kwq_sdt.c, kwq_ddb.c, kwq.d translator)
    kwq_test/     kwq_test.ko - synthetic clients driven by sysctl
    tests/        ATF harness (Kyuafile, kwq_lib.sh, t_*.sh) and
                  kwqvm.sh, which builds and runs the disposable
                  15.1/amd64 GENERIC-DEBUG bhyve guest the module tests
                  need (doas tests/kwqvm.sh setup; start; mods; ssh)
    examples/     compilable usage examples (deferred.c, scatter.c,
                  pair_client.c)
    man/          kwq.9, dtrace_kwq.4

Background research is in the parent directory: POOLS.md (cross-OS and
literature survey) and DISPATCH.md (libdispatch study).  The driver this
grew out of, and its measurements, are in ../if_pair.c and ../NOTES.md.

Build (once P0 lands): make -C kwq && make -C kwq_test; load with
kldload ./kwq/kwq.ko.  Tests: kyua test -k tests/Kyuafile.
