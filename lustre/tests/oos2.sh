#!/bin/bash

set -e

export PATH=`dirname $0`/../utils:$PATH
LFS=${LFS:-lfs}
MOUNT=${MOUNT:-$1}
MOUNT=${MOUNT:-/mnt/lustre}
MOUNT2=${MOUNT2:-$2}
MOUNT2=${MOUNT2:-${MOUNT}2}
OOS=$MOUNT/oosfile
OOS2=$MOUNT2/oosfile2
TMP=${TMP:-/tmp}
LOG=$TMP/oosfile
LOG2=${LOG}2

SUCCESS=1

rm -f $OOS $OOS2 $LOG $LOG2

sleep 1	# to ensure we get up-to-date statfs info

STRIPECOUNT=`cat /proc/fs/lustre/lov/*/activeobd | head -1`
ORIGFREE=`cat /proc/fs/lustre/llite/*/kbytesavail | head -1`
MAXFREE=${MAXFREE:-$((200000 * $STRIPECOUNT))}
if [ $ORIGFREE -gt $MAXFREE ]; then
	echo "skipping out-of-space test on $OSC"
	echo "reports ${ORIGFREE}kB free, more tham MAXFREE ${MAXFREE}kB"
	echo "increase $MAXFREE (or reduce test fs size) to proceed"
	exit 0
fi

export LANG=C LC_LANG=C # for "No space left on device" message

# make sure we stripe over all OSTs to avoid OOS on only a subset of OSTs
$LFS setstripe $OOS 65536 -1 $STRIPECOUNT
$LFS setstripe $OOS2 65536 -1 $STRIPECOUNT
dd if=/dev/zero of=$OOS count=$((3 * $ORIGFREE / 4 + 100)) bs=1k 2>> $LOG &
DDPID=$!
if dd if=/dev/zero of=$OOS2 count=$((3*$ORIGFREE/4 + 100)) bs=1k 2>> $LOG2; then
	echo "ERROR: dd2 did not fail"
	SUCCESS=0
fi
if wait $DDPID; then
	echo "ERROR: dd did not fail"
	SUCCESS=0
fi

if [ "`cat $LOG $LOG2 | grep -c 'No space left on device'`" -ne 2 ]; then
        echo "ERROR: dd not return ENOSPC"
	SUCCESS=0
fi

# flush cache to OST(s) so avail numbers are correct
sync; sleep 1 ; sync

for OSC in /proc/fs/lustre/osc/OSC*MNT*; do
	AVAIL=`cat $OSC/kbytesavail`
	GRANT=`cat $OSC/cur_grant_bytes`
	[ $(($AVAIL - $GRANT / 1024)) -lt 400 ] && OSCFULL=full
done
if [ -z "$OSCFULL" ]; then
	echo "no OSTs are close to full"
	grep [0-9] /proc/fs/lustre/osc/OSC*MNT*/{kbytesavail,cur*} |tee -a $LOG
	SUCCESS=0
fi

RECORDSOUT=$((`grep "records out" $LOG | cut -d+ -f 1` + \
              `grep "records out" $LOG2 | cut -d+ -f 1`))

FILESIZE=$((`ls -l $OOS | awk '{print $5}'` + `ls -l $OOS2 | awk '{print $5}'`))
if [ $RECORDSOUT -ne $(($FILESIZE / 1024)) ]; then
        echo "ERROR: blocks written by dd not equal to the size of file"
        SUCCESS=0
fi

rm -f $OOS $OOS2

if [ $SUCCESS -eq 1 ]; then
	echo "Success!"
else
	exit 1
fi
