#!/bin/sh
# deploy.sh HOST: if_kpair.ko on HOST, against its GENERIC objects, into
# ~/if_pair-scratch/p6g10/stage beside p6g9's modules (kwq and kwq-geli are
# unchanged since), with geli_bench and the scripts here.  Nothing is loaded.
set -e
H=${1:?HOST}; W=/home/crest/if_pair-scratch/wt/bulk
T=$(mktemp -d /tmp/kpair.XXXXXX); python3 $W/samples/p6g10/kpair.py $W/if_pair.c $T
ssh $H 'set -e; D=$HOME/if_pair-scratch/p6g10; P=$HOME/if_pair-scratch/p6g9
rm -rf $D; mkdir -p $D/stage $D/src/kpair $D/src/p6g10 $D/src/kwq-geli/tests; cp $P/stage/* $D/stage/'
scp -q $T/if_kpair.c $T/Makefile $H:if_pair-scratch/p6g10/src/kpair/
scp -q $W/kwq-geli/tests/geli_bench.sh $W/kwq-geli/tests/iolat.c $H:if_pair-scratch/p6g10/src/kwq-geli/tests/
scp -q $W/samples/p6g10/battery.sh $W/samples/p6g10/net_setup.sh $W/samples/p6g10/net_traffic.sh $H:if_pair-scratch/p6g10/src/p6g10/
ssh $H 'set -e; D=$HOME/if_pair-scratch/p6g10; K=$(ls -d /usr/obj/usr/src/*/sys/GENERIC | head -1)
cd $D/src/kpair; make -s SYSDIR=/usr/src/sys KERNBUILDDIR=$K KWQ_DIR=$HOME/if_pair-scratch/p6g9/src/kwq/kwq >/dev/null
cp if_kpair.ko $D/stage/; ls -l $D/stage | wc -l; uptime'
rm -rf $T
