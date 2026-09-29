/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * "show kwq" in DDB (../KWQ.md S10.5): every queue with its per-CPU depth
 * and state, then every worker's phase, round and current queue.  No locks
 * are taken; the lists may be inconsistent if the panic happened inside
 * kwq, which is the case the command is for.
 */

#include "opt_ddb.h"

#ifdef DDB
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/smp.h>
#include <ddb/ddb.h>

#include "kwq_internal.h"
#include "kwq_sched.h"

static const char *kwq_ddb_state[] = { "idle", "waking", "running", "parked" };
static const char *kwq_ddb_qstate[] = { "inactive", "active", "draining", "drained" };

DB_SHOW_COMMAND(kwq, db_show_kwq)
{
	struct kwq *q;
	struct kwq_cpu *kc;
	struct kwq_worker *kw;
	int cls, cpu, shown;

	LIST_FOREACH(q, &kwq_all, kwq_all) {
		db_printf("kwq %s class %s %s weight %u limit %u flags %#x\n",
		    q->kwq_name, kwq_class_names[q->kwq_class],
		    kwq_ddb_qstate[q->kwq_state & 3], q->kwq_weight,
		    q->kwq_limit, q->kwq_flags);
		shown = 0;
		CPU_FOREACH(cpu) {
			kc = q->kwq_pcpu[cpu];
			if (kc == NULL)
				continue;
			if (kc->kc_depth == 0 && kc->kc_nnotify == 0 &&
			    kc->kc_state == KWQ_CPU_IDLE && kc->kc_passes == 0)
				continue;	/* never used on this CPU */
			db_printf("  cpu%-3d %-7s depth %u notify %u deficit %jd"
			    " passes %ju items %ju overruns %ju\n", cpu,
			    kwq_ddb_state[kc->kc_state & 3], kc->kc_depth,
			    kc->kc_nnotify, (intmax_t)kc->kc_deficit,
			    (uintmax_t)kc->kc_passes, (uintmax_t)kc->kc_items,
			    (uintmax_t)kc->kc_overruns);
			shown++;
			if (db_pager_quit)
				return;
		}
		if (shown == 0)
			db_printf("  (idle on every CPU)\n");
	}
	for (cls = 0; cls < KWQ_NCLASS; cls++) {
		if (kwq_workers[cls] == NULL)
			continue;
		CPU_FOREACH(cpu) {
			kw = kwq_workers[cls][cpu];
			if (kw == NULL)
				continue;
			db_printf("worker kwq_%s/%d %s round %ju ring %u new %u"
			    " passes %ju cur %s\n", kwq_class_names[cls], cpu,
			    kw->kw_sleeping ? "asleep" :
			    kw->kw_phase == KWQ_PH_ROUND ? "in round" : "between rounds",
			    (uintmax_t)kw->kw_round, kw->kw_nactive, kw->kw_nnew,
			    (uintmax_t)kw->kw_passes,
			    kw->kw_cur != NULL ? kw->kw_cur->kc_q->kwq_name : "-");
			if (db_pager_quit)
				return;
		}
	}
}
#endif /* DDB */
