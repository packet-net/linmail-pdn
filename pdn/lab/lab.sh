#!/bin/sh
# linmail-pdn lab: a pdn node, a LinBPQ node with mail, and linmail-pdn, all on
# loopback. LinBPQ reaches pdn over AXUDP (UDP 19093 <-> 19094); linmail-pdn
# reaches pdn over RHPv2 (TCP 19000).
#
#   lab.sh start WORKDIR PACKETNET LINBPQ_BINARY
#   lab.sh stop WORKDIR
#
# PACKETNET is the packetnet binary from a pdn release, or packetnet.dll from
# a source build (run with dotnet).
#
# Ports used: pdn 18011 (telnet), 18080 (http), 19000 (rhp), 19094/udp;
# LinBPQ 18023 (telnet), 18088 (http), 19093/udp.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CMD=$1
WORK=$2

case "$CMD" in
start)
	DLL=$3
	LINBPQ=$4
	mkdir -p "$WORK/pdn" "$WORK/linbpq" "$WORK/linmail-pdn"
	for f in pdn/packetnet.yaml linbpq/bpq32.cfg linbpq/linmail.cfg linmail-pdn/linmail.cfg; do
		[ -f "$WORK/$f" ] || cp "$HERE/$f" "$WORK/$f"
	done

	case "$DLL" in
	*.dll) PDN="dotnet $DLL" ;;
	*) PDN="$DLL" ;;
	esac

	(cd "$WORK/pdn" && exec $PDN --config "$WORK/pdn/packetnet.yaml" --db "$WORK/pdn/pdn.db" \
		> "$WORK/pdn/pdn.log" 2>&1) &
	echo $! > "$WORK/pdn/pid"

	(cd "$WORK/linbpq" && exec "$LINBPQ" mail < /dev/null > "$WORK/linbpq/linbpq.log" 2>&1) &
	echo $! > "$WORK/linbpq/pid"

	sleep 10

	"$HERE/../linmail-pdn" -d "$WORK/linmail-pdn" -r 127.0.0.1:19000 -c N0LMB -n N0PDN -m 1=bpq -t \
		< /dev/null > "$WORK/linmail-pdn/linmail-pdn.log" 2>&1 &
	echo $! > "$WORK/linmail-pdn/pid"
	echo "lab started in $WORK"
	;;
stop)
	for p in linmail-pdn linbpq pdn; do
		[ -f "$WORK/$p/pid" ] && kill "$(cat "$WORK/$p/pid")" 2>/dev/null || true
		rm -f "$WORK/$p/pid"
	done
	;;
*)
	sed -n '2,15p' "$0"
	exit 1
	;;
esac
