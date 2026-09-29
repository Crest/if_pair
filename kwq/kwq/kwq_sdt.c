/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * kwq DTrace provider (../KWQ.md S10.1).  Queue arguments are declared with
 * the translator type kwqinfo_t (kwq.d) so scripts never see a raw struct
 * kwq pointer; plain arguments repeat their type, the form the tcp provider
 * uses for untranslated arguments.  Probes for P6 (steal) and P7 (worker
 * pool) are defined so that "dtrace -l" shows the whole set; they fire
 * once those phases exist.
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
/* list swapped: cpu, items, doorbell-to-pass latency ns (first pass only) */
SDT_PROBE_DEFINE4_XLATE(kwq, , , pass__start, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "uint64_t", "uint64_t");
/* handler returned: cpu, items, CPU time ns, items requeued */
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
/* class, cpu, reason (0 round, 1 tick) */
SDT_PROBE_DEFINE3(kwq, , , yield, "int", "int", "int");
/* class, cpu, busy ns since the last wakeup */
SDT_PROBE_DEFINE3(kwq, , , idle, "int", "int", "uint64_t");
/* P6: from cpu, to cpu, items */
SDT_PROBE_DEFINE4_XLATE(kwq, , , steal, "struct kwq *", "kwqinfo_t *",
    "int", "int", "int", "int", "int", "int");
/* P7: cpu, workers, runnable */
SDT_PROBE_DEFINE3(kwq, , , worker__block, "int", "int", "int");
SDT_PROBE_DEFINE3(kwq, , , worker__spawn, "int", "int", "int");
SDT_PROBE_DEFINE3(kwq, , , worker__exit, "int", "int", "int");
/* a handler asked for its budget and got 0: cpu */
SDT_PROBE_DEFINE2_XLATE(kwq, , , budget__hit, "struct kwq *", "kwqinfo_t *",
    "int", "int");
