#!/bin/sh
# build_guest.sh - if_kpair.ko against the guest's GENERIC-DEBUG objects, to
# the guest's /root/ifpair-scratch/stages/p6g10 with p6g9's modules (kwq and
# kwq-geli are unchanged since), geli_bench and the scripts here.
set -e
W=/home/crest/if_pair-scratch/wt/bulk
SRC=/bhyve/src-15.1; KO=/bhyve/obj-15.1$SRC/amd64.amd64/sys/GENERIC-DEBUG
SSHOPTS="-i $HOME/.ssh/id_kwqvm -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$HOME/.ssh/known_hosts_kwqvm"
G=/root/ifpair-scratch/stages/p6g10; P=/root/ifpair-scratch/stages/p6g9
T=$HOME/.kwqvm-obj-p6g10/src; rm -rf $T; python3 $W/samples/p6g10/kpair.py $W/if_pair.c $T
export MAKEOBJDIRPREFIX=$HOME/.kwqvm-obj-p6g10/obj; mkdir -p $MAKEOBJDIRPREFIX
make -C $T -s obj >/dev/null
make -C $T -s clean all SYSDIR=$SRC/sys KERNBUILDDIR=$KO KWQ_DIR=$W/kwq/kwq 2>&1 | grep -v '^$' || true
ssh $SSHOPTS root@10.0.99.2 "rm -rf $G; mkdir -p $G/stage $G/src/kwq-geli/tests $G/src/p6g10; cp $P/mods/* $P/gb/victim $G/stage/"
scp -q $SSHOPTS $(make -C $T -V .OBJDIR)/if_kpair.ko root@10.0.99.2:$G/stage/
scp -q $SSHOPTS $W/kwq-geli/tests/geli_bench.sh $W/kwq-geli/tests/iolat.c root@10.0.99.2:$G/src/kwq-geli/tests/
scp -q $SSHOPTS $W/samples/p6g10/battery.sh $W/samples/p6g10/net_setup.sh $W/samples/p6g10/net_traffic.sh root@10.0.99.2:$G/src/p6g10/
ssh $SSHOPTS root@10.0.99.2 "sync; ls -l $G/stage"
