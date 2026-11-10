/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq DTrace provider (../KWQ.md S10.1).  Queue arguments are declared with
 * the translator type kwqinfo_t (kwq.d) so scripts never see a raw struct
 * kwq pointer; plain arguments repeat their type, the form the tcp provider
 * uses for untranslated arguments.  The probes for P7 (worker
 * pool) are defined so that "dtrace -l" shows the whole set; they fire
 * once that phase exists.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/sdt.h>

SDT_PROVIDER_DEFINE(kwq);

SDT_PROBE_DEFINE1_XLATE(kwq, , , create, "struct kwq *", "kwqinfo_t *");
SDT_PROBE_DEFINE1_XLATE(kwq, , , activate, "struct kwq *", "kwqinfo_t *");
SDT_PROBE_DEFINE1_XLATE(kwq, , , drain__start, "struct kwq *", "kwqinfo_t *");
SDT_PROBE_DEFINE1_XLATE(kwq, , , drain__end, "struct kwq *", "kwqinfo_t *");
SDT_PROBE_DEFINE1_XLATE(kwq, , , destroy, "struct kwq *", "kwqinfo_t *");

/* every accepted enqueue call: cpu, depth after, 1 if it rang the doorbell */
SDT_PROBE_DEFINE4_XLATE(kwq, , , enqueue, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "int", "int");
/* refused enqueue or notify: cpu, errno */
SDT_PROBE_DEFINE3_XLATE(kwq, , , reject, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int");
/* list swapped: cpu, elements (notifiers included), doorbell-to-pass latency ns (first pass only) */
SDT_PROBE_DEFINE4_XLATE(kwq, , , pass__start, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "uint64_t", "uint64_t");
/* handler returned: cpu, elements, CPU time ns, elements requeued */
SDT_PROBE_DEFINE5_XLATE(kwq, , , pass__end, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "uint64_t", "uint64_t", "int", "int");
/* cpu, ns beyond the budget */
SDT_PROBE_DEFINE3_XLATE(kwq, , , overrun, "struct kwq *", "kwqinfo_t *",
    "int", "int", "uint64_t", "uint64_t");
/* cpu, deficit ns (negative) */
SDT_PROBE_DEFINE3_XLATE(kwq, , , park, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int64_t", "int64_t");
/* class, cpu, ring entries, wall ns since the previous round end */
SDT_PROBE_DEFINE4(kwq, , , round__end, "int", "int", "int", "uint64_t");
/* class, cpu, reason (KWQ_YIELD_*: 0 round, 1 tick, 2 share limit, 3 slice) */
SDT_PROBE_DEFINE3(kwq, , , yield, "int", "int", "int");
/*
 * The share limit (SCHED.md S6.3): class, cpu, event (KWQ_CAP_*), ns into
 * the period, ns used of it, and for a pass begin its budget in ns.
 */
SDT_PROBE_DEFINE6(kwq, , , cap, "int", "int", "int", "uint64_t", "uint64_t",
    "int64_t");
/* class, cpu, busy ns since the last wakeup */
SDT_PROBE_DEFINE3(kwq, , , idle, "int", "int", "uint64_t");
/*
 * class, cpu, the worker's state when rung (KWQ_DB_*: 0 awake, 2 asleep,
 * 4 made runnable in the idle class, 6 asleep with the tick armed; 1
 * polling is reserved and never fires); also fired by the urgent nudge
 */
SDT_PROBE_DEFINE3(kwq, , , doorbell, "int", "int", "int");
/*
 * a tick wake: class, cpu, elements served since the previous sleep (the
 * previous tick's round, or at an episode's first tick the round that
 * entered the regime) (KWQ.md S16)
 */
SDT_PROBE_DEFINE3(kwq, , , tick, "int", "int", "uint64_t");
/*
 * a worker waiting in the idle class is promoted: class, cpu, reason
 * (KWQ_WAKE_*), ns since the wait began (KWQ.md S16)
 */
SDT_PROBE_DEFINE4(kwq, , , wake, "int", "int", "int", "uint64_t");
/*
 * P6, SCHED.md S16-S17: a batch moved between two CPUs' lists of a
 * stealable queue: from cpu, to cpu, items, how (KWQ_STEAL_*), the items
 * left on the list it came from
 */
SDT_PROBE_DEFINE6_XLATE(kwq, , , steal, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "int", "int", "int", "int", "int", "int");
/*
 * a sleeping BULK worker woken to steal: the CPU it is to steal from, the
 * woken CPU (-1: no worker in reach slept), why (KWQ_SWAKE_*), 1 if the
 * wake named the list in its mailbox (place=2, shard.directed)
 */
SDT_PROBE_DEFINE4(kwq, , , steal__wake, "int", "int", "int", "int");
/*
 * a steal attempt that moved nothing: the thief's cpu, how (KWQ_STEAL_*),
 * the victim cpu (-1: none found), why (KWQ_SMISS_*)
 */
SDT_PROBE_DEFINE4(kwq, , , steal__miss, "int", "int", "int", "int");
/*
 * place=2: a CPU's donor or starved state changed: cpu, donor, starved
 * (the starvation watchdog's flag), the stealable backlog, ns
 */
SDT_PROBE_DEFINE4(kwq, , , donor, "int", "int", "int", "uint64_t");
/*
 * where a BULK item for KWQ_CPU_ANY, or a fixed CPU's spilled one, goes:
 * the producer's cpu, the chosen cpu, how (KWQ_PICK_*), 1 for a spill
 */
SDT_PROBE_DEFINE5_XLATE(kwq, , , place, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "int", "int", "int", "int");
/* P7: cpu, workers, runnable */
SDT_PROBE_DEFINE3(kwq, , , worker__block, "int", "int", "int");
SDT_PROBE_DEFINE3(kwq, , , worker__spawn, "int", "int", "int");
SDT_PROBE_DEFINE3(kwq, , , worker__exit, "int", "int", "int");
/* a handler asked for its budget and got 0: cpu */
SDT_PROBE_DEFINE2_XLATE(kwq, , , budget__hit, "struct kwq *", "kwqinfo_t *",
    "int", "int");
