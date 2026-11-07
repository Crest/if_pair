#!/bin/sh
# deploy.sh HOST: kwq.ko, kwq_test.ko, kwq_crypto.ko, geom_kwqeli.ko and .so,
# victim of the kwq-bulk worktree as it stands, built on HOST against its
# GENERIC objects, into ~/if_pair-scratch/p6g11/stage, with if_kpair.ko from
# p6g10 (its source has not changed) and the scripts here.  Nothing is
# installed or loaded.  The build runs under sh: lab's and a07's login shell
# is zsh, which does not split $A.
set -e
H=${1:?HOST}; W=/home/crest/if_pair-scratch/wt/bulk
cd $W
[ -z "$(ssh $H 'pgrep -f p6g11/battery; true')" ] || { echo "a battery runs on $H: not deploying"; exit 1; }
ssh $H 'D=$HOME/if_pair-scratch/p6g11; rm -rf $D/src $D/stage; mkdir -p $D/src $D/stage'
tar cf - kwq/kwq kwq/kwq_test kwq/kwq_crypto kwq/tests kwq-geli tests/victim.c samples/p6g10/net_setup.sh samples/p6g10/net_traffic.sh samples/p6g11/battery.sh samples/p6g11/battery2.sh samples/p6g11/nettime.sh samples/p6g11/diag.sh samples/p6g11/home_sweep.sh | ssh $H 'tar -xf - -C $HOME/if_pair-scratch/p6g11/src'
ssh $H sh -s <<'BUILD'
set -e
D=$HOME/if_pair-scratch/p6g11; K=$(ls -d /usr/obj/usr/src/*/sys/GENERIC | head -1); A="SYSDIR=/usr/src/sys KERNBUILDDIR=$K"
cd $D/src/kwq/kwq; make -s $A >/dev/null; cp kwq.ko $D/stage/
cd ../kwq_test; make -s $A >/dev/null; cp kwq_test.ko $D/stage/
cd ../kwq_crypto; make -s $A >/dev/null; cp kwq_crypto.ko $D/stage/
cd ../../kwq-geli/sys; make -s $A >/dev/null; cp geom_kwqeli.ko $D/stage/
cd ../lib; make -s SRCTOP=/usr/src >/dev/null; cp geom_kwqeli.so $D/stage/
cd ../tests/simdisk; make -s $A >/dev/null; cp geom_simdisk.ko $D/stage/
cd $D/src/tests; cc -O2 -o $D/stage/victim victim.c -lm
cp $HOME/if_pair-scratch/p6g10/stage/if_kpair.ko $D/stage/
ls $D/stage | tr "\n" " "; echo; uptime
BUILD
