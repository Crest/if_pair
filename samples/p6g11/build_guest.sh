#!/bin/sh
# build_guest.sh - kwq.ko (KWQ_LIST_CHECK=1), kwq_test.ko, kwq_crypto.ko
# and geom_kwqeli.ko of the kwq-bulk worktree against the guest's
# GENERIC-DEBUG objects, with the suites, geli_bench and victim.c, to the
# guest's /root/ifpair-scratch/stages/p6g11 (kwq-geli's geom_kwqeli.so,
# unchanged since 54d61e5, from stages/p6g2/geli).
set -e
W=/home/crest/if_pair-scratch/wt/bulk
SRC=/bhyve/src-15.1; KO=/bhyve/obj-15.1$SRC/amd64.amd64/sys/GENERIC-DEBUG
SSHOPTS="-i $HOME/.ssh/id_kwqvm -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$HOME/.ssh/known_hosts_kwqvm"
G=/root/ifpair-scratch/stages/p6g11
export MAKEOBJDIRPREFIX=$HOME/.kwqvm-obj-p6g11; mkdir -p $MAKEOBJDIRPREFIX
for d in kwq/kwq kwq/kwq_crypto kwq/kwq_test kwq-geli/sys kwq-geli/tests/simdisk; do
	a=; [ $d = kwq/kwq ] && a=KWQ_LIST_CHECK=1
	make -C $W/$d -s obj >/dev/null
	make -C $W/$d -s clean all SYSDIR=$SRC/sys KERNBUILDDIR=$KO $a 2>&1 | grep -v '^$' || true
done
ssh $SSHOPTS root@10.0.99.2 "rm -rf $G; mkdir -p $G/mods $G/kwq-tests $G/geli-tests $G/gb/kwq-geli/tests $G/gb/tests"
scp -q $SSHOPTS $(make -C $W/kwq/kwq -V .OBJDIR)/kwq.ko $(make -C $W/kwq/kwq_test -V .OBJDIR)/kwq_test.ko \
    $(make -C $W/kwq/kwq_crypto -V .OBJDIR)/kwq_crypto.ko $(make -C $W/kwq-geli/sys -V .OBJDIR)/geom_kwqeli.ko \
    $(make -C $W/kwq-geli/tests/simdisk -V .OBJDIR)/geom_simdisk.ko root@10.0.99.2:$G/mods/
scp -q $SSHOPTS $W/kwq/tests/kwq_test.sh $W/kwq/tests/kwq_crypto_test.sh $W/kwq/tests/Kyuafile root@10.0.99.2:$G/kwq-tests/
scp -q $SSHOPTS $W/kwq-geli/tests/kwqeli_test.sh $W/kwq-geli/tests/Kyuafile root@10.0.99.2:$G/geli-tests/
scp -q $SSHOPTS $W/kwq-geli/tests/geli_bench.sh $W/kwq-geli/tests/iolat.c root@10.0.99.2:$G/gb/kwq-geli/tests/
scp -q $SSHOPTS $W/tests/victim.c root@10.0.99.2:$G/gb/tests/
scp -q $SSHOPTS $W/samples/p6g11/*.sh root@10.0.99.2:$G/
ssh $SSHOPTS root@10.0.99.2 "cp /root/ifpair-scratch/stages/p6g2/geli/geom_kwqeli.so $G/mods/; sync; ls -l $G/mods"
