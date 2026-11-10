# gelisim

kwq-geli on many CPUs in user space, to try changes to where a pass may
stop before any of them reaches a kernel (2026-11-06).  The model is
kwq-geli of that day.  What followed in the kernel: the time rule became
`kern.kwq.<class>.item_min_us` (BULK 2000; 651a7e9), and kwq-geli's
write lost its copy and its reads got a queue of their own (d25b01f),
which are not modelled; ../../BULK.md S7.7.  It links the real scheduler
core, `../../kwq/kwq_sched.c`, with `keepturn.patch` applied at build
time; with the knob off the patched core gives byte-identical output in
`../kwqsim suite`.

    make
    ./gelisim nprov=64 keep=2 trule_us=1000 timer=1
    python3 run.py out/runs.jsonl 7     # 1981 runs, a few minutes
    python3 summarize.py out/runs.jsonl scale reads

Options are `key=value`, listed in `struct opts`; `keep=1` is the full
keep-turn, `keep=2` keeps the turn for NET cuts only, `trule_us` is the
time rule's T, `fit=1` the fit test, `shared=1` one queue for all
providers, `owner=1` the GELI-only owner.  "sectors 16" and "sectors 0"
below are kwq-geli's `kern.geom.kwqeli.budget_sectors`.

## The hardware classes in run.py

Costs are per MiB unless named otherwise.

- `LAB`: lab at 32 providers and more, where all 16 logical CPUs are busy:
  crypto 0.5 ms, copy 0.5 ms, md(4) 0.5 ms and 0.47 ms of other work, the
  1.97 ms per MiB measured at `sectors 0`.  Fitted to lab: 2000 short kernel
  wake-ups per CPU per second and a cold-cache factor of 3 (the profile
  alone suggests 0.9).
- `LAB16`: lab at 16 providers, the 1.35 ms per MiB measured there.
- `PIN_LAB`, `PIN_A07`: one CPU pinned as `onecpu` and `netcpu`, quiet costs.
- `A07`: not calibrated at scale.  The model has no memory or lock limit,
  so it reaches 51-65 GB/s where a07 measures about 7; only pinned a07
  results mean anything.
- `N1SW`, `SLOW`: 6.7 ms and 60 ms per bio, estimated from FreeBSD's software
  AES path on a07 (2026-11-06); no in-order CPU was measured.

Cost jitter is +-10 %.  Without it items that divide the allowance lock
into the period more often than they should.

## What it does not model

Real disks, memory and lock limits, SMT, NUMA, the P8 shards and ULE's
load balancer; stealing and KWQ_CPU_ANY are simplified.  Dense NET
throughput does not reproduce test 2: the model gives NET half the CPU at
`sectors 16` and `sectors 0` alike.
