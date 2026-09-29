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
