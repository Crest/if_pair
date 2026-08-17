#!/bin/sh
# cleanup.sh - sweep state left behind by failed if_pair tests:
# every test jail (ifp_*) and every member of the "pair" interface
# group.  Note the group sweep destroys ALL pair interfaces, not
# just test-created ones.  The module is left loaded.
for j in $(jls name 2>/dev/null | grep '^ifp_'); do
	echo "removing jail $j"
	jail -r "$j"
done

# Jail teardown is asynchronous; moved sides come home when their
# dying vnet is destroyed, so retry the group sweep for a while.
tries=0
while [ "$tries" -lt 60 ]; do
	m=$(ifconfig -g pair 2>/dev/null | head -1)
	[ -z "$m" ] && break
	echo "destroying $m"
	ifconfig "$m" destroy 2>/dev/null || sleep 1
	tries=$((tries + 1))
done
if [ -n "$(ifconfig -g pair 2>/dev/null)" ]; then
	echo "warning: pair group still has members" >&2
	exit 1
fi

if kldstat -q -m if_pair; then
	echo "if_pair is still loaded; 'kldunload if_pair' removes it"
fi
