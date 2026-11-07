#!/usr/bin/env python3
"""kpair.py IF_PAIR.C OUTDIR: if_pair.c as a module that loads beside another
if_pair: module if_kpair, interfaces kpair<N>a/b, sysctls under
net.link.kpair (a07's loaded if_pair.ko is crest's and stays).  One addition,
for the test only: net.link.kpair.cpu, when not -1 the CPU every packet goes
to, so that a NET worker can be put on the CPU of a BULK worker; a flow's CPU
otherwise comes from a hash with a random seed."""
import os, sys

src, out = sys.argv[1], sys.argv[2]
s = open(src).read()

def rep(a, b, n=1):
    global s
    assert s.count(a) == n, (a, s.count(a))
    s = s.replace(a, b)

rep('#define\tPAIR_NAME\t"pair"', '#define\tPAIR_NAME\t"kpair"')
rep('"if_pair"', '"if_kpair"', 2)
rep('DECLARE_MODULE(if_pair,', 'DECLARE_MODULE(if_kpair,')
rep('MODULE_VERSION(if_pair,', 'MODULE_VERSION(if_kpair,')
rep('MODULE_DEPEND(if_pair,', 'MODULE_DEPEND(if_kpair,')
rep('SYSCTL_NODE(_net_link, OID_AUTO, pair,', 'SYSCTL_NODE(_net_link, OID_AUTO, kpair,')
s = s.replace('_net_link_pair', '_net_link_kpair')
rep('''static int
pair_select_cpu(struct pair_softc *sc, struct mbuf *m, uint32_t af)
{
	uint32_t hash;

''', '''static int pair_force_cpu = -1;
SYSCTL_INT(_net_link_kpair, OID_AUTO, cpu, CTLFLAG_RW, &pair_force_cpu, 0,
    "every packet to this CPU; -1: by flow hash (test only)");

static int
pair_select_cpu(struct pair_softc *sc, struct mbuf *m, uint32_t af)
{
	uint32_t hash;

	if (pair_force_cpu >= 0)
		return (pair_force_cpu);
''')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'if_kpair.c'), 'w').write(s)
open(os.path.join(out, 'Makefile'), 'w').write('''KMOD=	if_kpair
SRCS=	if_kpair.c
SRCS+=	opt_inet.h opt_inet6.h
CFLAGS+=	-I${KWQ_DIR}

.include <bsd.kmod.mk>
''')
