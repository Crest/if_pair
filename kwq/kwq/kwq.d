/*
 * kwq DTrace translator (../KWQ.md S10.2).  Install as
 * /usr/lib/dtrace/kwq.d, or pass -L <this directory> to dtrace(1).
 */

#pragma D depends_on module kwq
#pragma D depends_on provider kwq

typedef struct kwqinfo {
	string kwq_name;	/* "pair0a", "netisr_ip", "wg_crypto" */
	string kwq_class;	/* "net", "bulk", "blocking" */
	int kwq_weight;
	uint32_t kwq_limit;	/* per-CPU bound */
	uint32_t kwq_flags;
	uintptr_t kwq_addr;	/* for correlating with lockstat/fbt */
} kwqinfo_t;

#pragma D binding "1.13" translator
translator kwqinfo_t < struct kwq *Q > {
	kwq_name = Q == NULL ? "<none>" : stringof(Q->kwq_name);
	kwq_class = Q == NULL ? "<none>" :
	    Q->kwq_class == 0 ? "net" : Q->kwq_class == 1 ? "bulk" : "blocking";
	kwq_weight = Q == NULL ? 0 : Q->kwq_weight;
	kwq_limit = Q == NULL ? 0 : Q->kwq_limit;
	kwq_flags = Q == NULL ? 0 : Q->kwq_flags;
	kwq_addr = (uintptr_t)Q;
};

/* The probes' codes, as kwq_sdt.h defines them (../OBSERVABILITY.md). */

/* the batch's path: kwq:::steal arg4, kwq:::steal-miss arg1 */
inline int KWQ_STEAL_SCAN = 0;
#pragma D binding "1.13" KWQ_STEAL_SCAN
inline int KWQ_STEAL_DONOR = 1;
#pragma D binding "1.13" KWQ_STEAL_DONOR
inline int KWQ_STEAL_MAILBOX = 2;
#pragma D binding "1.13" KWQ_STEAL_MAILBOX
inline int KWQ_STEAL_CHAIN = 3;
#pragma D binding "1.13" KWQ_STEAL_CHAIN
inline int KWQ_STEAL_PULL = 4;
#pragma D binding "1.13" KWQ_STEAL_PULL
inline int KWQ_STEAL_HANDOFF = 5;
#pragma D binding "1.13" KWQ_STEAL_HANDOFF

/* why a sleeper is woken to steal: kwq:::steal-wake arg2 */
inline int KWQ_SWAKE_ENQUEUE = 0;
#pragma D binding "1.13" KWQ_SWAKE_ENQUEUE
inline int KWQ_SWAKE_HELPER = 1;
#pragma D binding "1.13" KWQ_SWAKE_HELPER
inline int KWQ_SWAKE_DONOR = 2;
#pragma D binding "1.13" KWQ_SWAKE_DONOR
inline int KWQ_SWAKE_WATCHDOG = 3;
#pragma D binding "1.13" KWQ_SWAKE_WATCHDOG

/* why an attempt moved nothing: kwq:::steal-miss arg3 */
inline int KWQ_SMISS_NONE = 0;
#pragma D binding "1.13" KWQ_SMISS_NONE
inline int KWQ_SMISS_STALE = 1;
#pragma D binding "1.13" KWQ_SMISS_STALE
inline int KWQ_SMISS_EMPTY = 2;
#pragma D binding "1.13" KWQ_SMISS_EMPTY
inline int KWQ_SMISS_LONE = 3;
#pragma D binding "1.13" KWQ_SMISS_LONE
inline int KWQ_SMISS_INACTIVE = 4;
#pragma D binding "1.13" KWQ_SMISS_INACTIVE

/* how the CPU was found: kwq:::place arg3 (KWQ_PICK_GOSSIP + KP_STEP_*) */
inline int KWQ_PICK_TODAY = 0;
#pragma D binding "1.13" KWQ_PICK_TODAY
inline int KWQ_PICK_NEAR = 1;
#pragma D binding "1.13" KWQ_PICK_NEAR
inline int KWQ_PICK_FAR = 2;
#pragma D binding "1.13" KWQ_PICK_FAR
inline int KWQ_PICK_SELF = 3;
#pragma D binding "1.13" KWQ_PICK_SELF
inline int KWQ_PICK_NONDONOR = 4;
#pragma D binding "1.13" KWQ_PICK_NONDONOR
inline int KWQ_PICK_PROBE = 5;
#pragma D binding "1.13" KWQ_PICK_PROBE
inline int KWQ_PICK_GOSSIP = 16;
#pragma D binding "1.13" KWQ_PICK_GOSSIP
