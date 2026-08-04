#!/bin/sh
RPROC=/sys/class/remoteproc/remoteproc0
systemctl stop mavlink-rpmsg-bridge 2>/dev/null
sleep 2
echo stop > $RPROC/state 2>/dev/null
sleep 4
systemctl start mavlink-rpmsg-bridge
sleep 3
echo nuttx.elf > $RPROC/firmware
echo start > $RPROC/state
sleep 20
echo "R5 state: $(cat $RPROC/state)  nodes: $(ls /dev/rpmsg* 2>/dev/null | grep -c rpmsg[0-9])"
