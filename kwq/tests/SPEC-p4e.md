# Test specifications for the wake-moderation follow-up (PLAN P4e)

2026-10-27.  What the two P4e campaigns (WAKE.md S7-S9) could not measure,
what they measured badly, and what the tick regime, the priority
invariant and the accounting question need before any of them is
decided.  Each specification states the hypothesis it tests and the
value the lower bound predicts, so the pass criterion is "within x of
the prediction", not "better than last time".

Conventions for every test below:

- One boot per machine, stages interleaved per repetition, at least
  three repetitions, medians; uptime recorded before and after on a07
  and lab.  The guest (GENERIC-DEBUG) runs every test for correctness
  (ATF, assertions, counter identities) and reports direction only; a
  magnitude is lab's or a07's.
- One kwq_test.ko for every kwq.ko stage of a battery: the newest
  build's.  The sweep of 2026-10-25 paired the batch build with its own
  test module, which lacked `prod_prio`, and reported a cross-CPU run
  as a same-CPU one; stage directories carry only kwq.ko and the pair
  drivers, the test module comes from one place.
- Counters are read as deltas over the measured window and reported
  per second; every row of a test is written to the TSV once per
  repetition; compare_report.sh renders medians.
- A test that needs dtrace says so and is skipped where dtrace is
  absent; nothing else may leave a consumer running (it vetoes the
  unload of the next stage).

## 1. Hooks in kwq the tests depend on

These are small and belong on the branch that implements the tick;
the tests cannot be written against the counters that exist today.

1. `kern.kwq.<class>.cpu.<N>.ticks`, `ticks_empty`, `ticks_single`
   (ticks that served one element: the exit condition), `tick_enters`,
   `tick_exits`, `tick_ns` (wall time spent in tick mode).  Per queue
   per CPU: `doorbells` (rings performed) and `doorbells_skipped`
   (appends made while the tick was armed).  Identity:
   `wakeups == doorbells performed by producers + ticks` within the
   window.
2. `overruns_switched` beside `overruns` per queue per CPU: an overrun
   in a pass during which the worker left the CPU (`switched` is
   already detected per pass).  `overruns - overruns_switched` is the
   handler's own excess.
3. `kern.kwq.<class>.charge`: `wall` (today's NET), `cpu` (thread CPU
   time, BULK's method), `split` (deficit in CPU time, round and share
   limit in wall time), `hybrid` (wall unless the pass was switched
   out, then CPU time).  RWTUN, read at pass begin so a stage may set
   it after load.
4. Under INVARIANTS, at every pass start: `KASSERT(td_priority <=
   kw_pri && td_pri_class == kw_pri_class)` (the priority may be lent
   higher, never lower; the class restored after an idle-class wait),
   and a counter `prio_violations` that counts instead of panicking
   when the module is built with `KWQ_SOFT_INVARIANTS`, so the ATF case
   can read it.
5. `kern.kwq_test.*` gains the knobs of section 2.  `prod_prio` and a
   producer on the consumer's CPU (`pcpu == cpu`) move from
   kwq-wake-prio into the main tree's kwq_test.

## 2. kwq_test scenarios

### 2.1 `burst`: a producer with a duty cycle

Hypothesis: the tick regime charges a dense burst one wake plus one
timer per bound L and leaves an isolated item alone; the price of a
burst boundary is one wake and at most one empty tick.

Knobs: `burst_items` B (default 64), `burst_gap_us` G (default 500),
`secs`, `cpu`, `pcpu`, `prod_prio`, `prod_ns` (work per item,
default 0), `cons_ns` (handler work per item, default 500).

Procedure: one queue, one producer; the producer enqueues B items
back to back (with `prod_ns` of work between them when set), then
sleeps G (pause_sbt with precision G/8 for G >= 100 us, a bounded spin
below), and repeats for `secs`.  Items carry their enqueue stamp; the
handler records the age of the first and the last item of each pass.
A burst's first-item latency is the age of the first item served
after a gap; its last-item latency the age of the burst's last item
when served.

Rows (per run, from the scenario's result sysctls and the worker's
counter deltas): bursts, items; first-item latency p50/p99/max; last-
item latency p50/p99/max; wakeups per burst; ticks per burst;
ticks_empty per burst; doorbells_skipped per burst; passes per burst;
worker idle share (idle_ns / wall); the worker CPU's timer interrupts
per burst (the per-CPU source of 3.5).

Matrix: (B, G) in {(1, 2000), (4, 200), (16, 200), (64, 500),
(64, 2000), (64, 50000), (1024, 5000)}; producer placements cross-CPU,
same CPU at `prod_prio` 100, and on lab the SMT sibling; bound L in
{0, 20, 50} through the class knob of the build under test.  Baseline
W0 beside every candidate.

Predictions and pass criteria, per (B, G) with L set:
- G > L and B = 1: ticks per burst 0, wakeups per burst 1, first-item
  latency equal to W0's within 1 us (tick mode must not enter on an
  isolated item).
- B >= 4 and the burst's duration exceeds L: wakeups per burst 1,
  ticks per burst within one of ceil(duration / L), ticks_empty per
  burst at most 1, passes per burst at most ticks + 1, last-item
  latency at most L plus one pass.
- G < L (bursts closer than the bound): wakeups per second at most
  1/L, i.e. the producer never rings while the tick is armed;
  doorbells_skipped equals items minus wakes.
- Worker idle share within 2 points of W0's when G >= 2 ms (nothing
  may stay armed across a silence).

### 2.2 `intr_load`: interrupt context on the worker CPU

Hypothesis: the priority invariant leaves exactly one preemptor, the
interrupt itself; its effect on accounting is what `overruns_switched`
and the `charge` modes have to show.

Not a scenario but a stressor usable under any scenario: knobs
`intr_cpu` (-1 off), `intr_us` (period), `intr_work_ns` (busy-wait in
the handler, at most 200 us), `intr_secs`.  Implementation: a
`C_DIRECT_EXEC` callout re-armed from its own handler on `intr_cpu`,
spinning `intr_work_ns` with `DELAY()` in interrupt context; the duty
cycle is refused above 50 %; the stressor stops itself after
`intr_secs` and when the module unloads (callout_drain).  Result
sysctls: fired, late (handlers that found the previous one not yet
finished), total work ns.

Rows when combined with another scenario: that scenario's rows plus
fired/s, and the worker's `switched` and the queue's `overruns`,
`overruns_switched`, `parks`, `debts` deltas.

Matrix: (period, work) in {(100 us, 20 us), (50 us, 20 us),
(1000 us, 200 us)} on the worker CPU, under `cost`, `fairness`
(two queues of unequal cost) and `burst`.

Pass criteria: with `charge=cpu`, `split` or `hybrid`, the well-behaved
queue's `overruns - overruns_switched` is 0 and its `parks` equal the
unstressed run's within 5 %; with `charge=wall` the same counters rise
in proportion to the stressor's duty cycle (this is the measurement
of the punishment, reported as parks per second of stress).  Under
`fairness`, the two queues' cycle ratio stays within 10 % of the
weight ratio in every mode except `wall`, where the deviation is
reported.

### 2.3 `ifpair` extension: sparse senders

Hypothesis: with many sparse producers on one worker the per-worker
tick amortises over all of them, and the per-wake interrupt of the
first campaign disappears.

Knob: `prod_gap_us` (0 today: flat out); each sender sleeps that long
between packets (pause_sbt, precision gap/8).  With `fanin` senders
at gap g the offered rate per worker is fanin / g.

Matrix: fanin in {1, 8, 64} (a07 up to 64, lab up to 14, guest 2),
gap in {20, 100, 1000} us, `prod_ns` 0, `cons_ns` 500, L in {0, 20,
50}.

Rows: as the ifpair scenario's today (pkt/s, enqueue ns and share of
sender CPU, worker share in packet work, queue latency mean/max) plus
wakeups/s, ticks/s, ticks_empty/s, items per pass, the worker CPU's
timer interrupts/s.

Predictions: wakeups/s + ticks/s within 20 % of lambda / (1 + lambda L)
with lambda the offered rate; items per pass within 20 % of 1 +
lambda L; queue latency max at most L plus one pass plus the timer
precision; the sender's enqueue cost in tick mode within 100 ns of the
`batch 16` row's per-item cost (the append without a doorbell).

### 2.4 `conformance`: the lower bound as a test

Hypothesis: for any arrival process the tick regime's pass count is
within a constant of the interval-stabbing bound.

Procedure: one queue, one cross-CPU producer, arrivals drawn from
three processes at rate lambda: constant spacing, exponential
spacing (a per-item random sleep), and on/off bursts; lambda in
{10 k, 100 k, 1 M} per second; L in {10, 20, 50, 100} us.  Rows:
passes/s, items per pass, maxlat, ticks/s, the three predictions
beside them.  Pass: passes/s at most 1.5 x lambda / (1 + lambda L)
for the constant and exponential processes, items per pass at least
0.7 x (1 + lambda L), maxlat at most L + pass + precision in every
process.  This is the row a change to the tick is judged by first.

## 3. compare.sh tests

### 3.1 `sparse`: many request-reply connections

The case neither tput16 nor machine covers.  N UDP request-reply
flows (victim.c's udp mode, each at r requests per second) from the
client jail to N echo servers in the server jail, source ports chosen
so that the flows spread over all workers (victim's ports mode with a
CPU list), N in {16, 64, 256} capped at 4 x ncpu, r in {1 k, 5 k}.
Rows: RTT p50/p99 per flow aggregated as min / median / max over
flows, lost requests, the sum over all NET workers of wakeups/s,
ticks/s and passes/s, items per pass (sum of items over sum of
passes), total interrupts/s and the sum of the per-CPU timer lines,
and the machine-wide system CPU share from kern.cp_times.  Stages:
W0, T and the candidate at L in {0, 20, 50}.  Pass: p50 RTT grows by
at most 2 L + 2 us over W0; wakes plus ticks per second fall by at
least the factor 1 + lambda_w L predicts for the per-worker rate
lambda_w = N r / ncpu; system CPU share does not rise.

### 3.2 `udp_burst`: bursty request-reply through the pair

The victim's udp mode gains a burst mode: B requests back to back,
then a gap G, repeating; it records each request's RTT with its index
in the burst.  Rows: first-in-burst RTT p50/p99, last-in-burst RTT
p50/p99, lost, and the worker's wakeups, ticks and ticks_empty per
burst.  Matrix: (B, G) in {(1, 2 ms), (16, 200 us), (64, 2 ms),
(64, 50 ms)}, with nothing else running and beside the bulk flow on
the worker CPU.  Pass: the (1, 2 ms) row equals udp_idle's W0 within
1 us at every L (isolated requests never enter tick mode); the others
follow the `burst` scenario's predictions.

### 3.3 `coloc`, corrected and extended

Already corrected: sixteen client ports instead of two, against the
TIME_WAIT reuse that lost one repetition per machine.  Add the rows
the P4d `twoq` test has: `overruns`, `overruns_switched`, `parks`,
`debts` of both pair queues on W; and the receiver process's own
involuntary context switches (procstat -r / getrusage through the
victim) so the preemption count has a user-side witness.

### 3.4 `twoq` and the P4d tests under the accounting modes

Every P4d test (`preempt`, `coloc`, `rwlock`, `xcpu`, `twoq`, `churn`,
`magnet`, `fair`) runs once per `charge` mode as separate stages of
the same build (the knob is a stage sysctl), under the `timer`,
`burst` and the new `intr` competitor.  `twoq` reports
`overruns_switched` beside `overruns` for both queues, and the items
share.  `COMPETITOR=intr` loads the battery's kwq_test.ko and arms
`intr_load` on W with (100 us, 20 us) for the test's window; it is
the interrupt-thread stressor the invariant permits by design.  Pass,
per mode: the well-behaved queue's clean overruns are 0; parks and
debts under stress within 10 % of the unstressed stage for `cpu`,
`split` and `hybrid`; the UDP flow's p99 beside the bulk flow (the
other consumers' latency) within 10 % of W0 for `wall`, `split` and
`hybrid`, reported for `cpu` (the hypothesis is that `cpu` lets a
blocked queue hold its quantum at the others' expense, and `split`
does not).

### 3.5 `machine`: interrupts per CPU

`intr_s` is machine-wide today.  Add `intr_w_timer_s`, the timer
interrupts of the worker CPU alone, and `intr_timer_s`, the sum over
CPUs.  On amd64 (lab, the guest) vmstat -i has one `cpuN:timer` line
per CPU and the deltas are read from it; on arm64 (a07) the generic
timer is one `gic0,...:-ic_timer0` line for all CPUs and the
`cpuN:hardclock` lines count hardclock callbacks, not interrupts, so
there the per-CPU figure comes from a dtrace count of the event
timer's interrupt handler by CPU (`fbt::handleevents:entry /cpu ==
W/`), and the test says which source it used.  Prediction under the
tick: `intr_w_timer_s` at most hz + 1/L while the flow runs.

### 3.6 The kwq rows of every test

`kwq_counters` reads, beside today's fields: ticks, ticks_empty,
ticks_single, doorbells_skipped (summed over the pair's two queues on
W), overruns and overruns_switched (both queues), prio_violations
(must be 0 in every row).  `kwq_rows` emits them per second, and
`kwq_items_per_pass` keeps its definition.

## 4. The row runner moves into the tree

`bench/wake_rows.sh` and `rows_report.py` of the campaign become
`tests/kwq_rows.sh` and `tests/kwq_rows_report.py`, with: an absolute
`RUNNER` default; `ROWS` selection (fanin, cost, pingpong, ifpair,
coloc, burst, sparse, conformance); the remote helper always through
`$KWQ_SSH 'sh -s'` with a heredoc (the guest's relative `kwqvm.sh ssh`
lost every counter line on 2026-10-24); the final unload through
`$KWQ_SSH 'sh -c "..."'` (a quoted command string is not a command
for doas or sudo); the counter line carrying every branch's counters
(0 where absent) and the new ones of section 1; a stage file syntax
`LABEL MODDIR [knob ...]` where MODDIR holds kwq.ko only and the test
module path is a separate `TESTMOD` variable; and the summariser's
tables for burst, sparse and conformance beside today's.

## 5. ATF cases (kwq/tests/kwq_test.sh)

- `prio_invariant`: ifpair with every moderation knob on (`wake_prio`,
  `wake_idle`, the tick bound, `cap_permille` 500) and `intr_load`
  active, 10 s; asserts `prio_violations` 0 on every worker and no
  WITNESS output.  Skipped on a kernel without INVARIANTS.
- `tick_hysteresis`: `burst` with (1, 2 ms): asserts `tick_enters` 0;
  then (16, 200 us): asserts `tick_enters` >= 1 and `ticks_empty` <=
  bursts; then a 500 ms silence: asserts `ticks` stopped advancing
  within L of the last item and the worker's idle sleep had no
  timeout (a `ticks` delta of 0 across the silence).
- `charge_modes`: a handler that blocks 200 us on a sleep mutex held by
  a stressor thread, once per pass, 2 s per mode: asserts the queue's
  clean overruns are 0 in every mode, `overruns_switched` > 0 in
  `wall` and `split`'s round accounting, `parks` 0 in `cpu` and
  `hybrid`.
- `no_arm_when_idle`: after `burst` ends, a 2 s silence: asserts the
  worker CPU's timer interrupt count (vmstat -i delta) is within hz of
  what the silence would produce on an idle CPU on that machine
  (measured in the same case with the module unloaded).
- The existing `gaming` case fails once per full run with "collected
  N boosts" and passes alone (2026-10-24, three times): give it a
  retry of one before reporting, and record the flake in the case's
  description.

## 6. Order and effort

1. Section 1's hooks and the test module's `prod_prio` move, with
   section 4's runner: a day; nothing else can start without them.
2. `burst` (2.1) and `udp_burst` (3.2): the duty-cycle producer is the
   measurement the campaign lacked most; a day including the victim's
   burst mode.
3. `intr_load` (2.2), the accounting modes (3.4) and `charge_modes`
   (5): together, since they are one question; a day.
4. `sparse` (3.1) and the ifpair gap (2.3): half a day.
5. `conformance` (2.4), `machine` per-CPU interrupts (3.5), the ATF
   cases: half a day.

Running the full set is about an hour per machine per candidate; the
first run should be W0 against the tick branch alone, so that the
predictions of 2.1, 2.3 and 2.4 are checked before any other variant
is added to the matrix.
