#!/bin/bash

set -e
set -vx

export PATH=`dirname $0`/../utils:$PATH
LFS=${LFS:-lfs}
MOUNT=${MOUNT:-$1}
MOUNT=${MOUNT:-/mnt/lustre}
OOS=$MOUNT/oosfile
TMP=${TMP:-/tmp}
LOG=$TMP/ooslog

SUCCESS=1

rm -f $OOS

sleep 1	# to ensure we get up-to-date statfs info

#echo -1 > /proc/sys/portals/debug
#echo 0x40a8 > /proc/sys/portals/subsystem_debug
#lctl clear
#lctl debug_daemon start /r/tmp/debug 1024

STRIPECOUNT=`cat /proc/fs/lustre/lov/*/activeobd | head -n 1`
ORIGFREE=`cat /proc/fs/lustre/llite/*/kbytesavail | head -n 1`
MAXFREE=${MAXFREE:-$((200000 * $STRIPECOUNT))}
if [ $ORIGFREE -gt $MAXFREE ]; then
	echo "skipping out-of-space test on $OSC"
	echo "reports ${ORIGFREE}kB free, more tham MAXFREE ${MAXFREE}kB"
	echo "increase $MAXFREE (or reduce test fs size) to proceed"
	exit 0
fi

export LANG=C LC_LANG=C # for "No space left on device" message

# make sure, that log file will be removed. Somehow it was possible 
# to me, that log file had +a and could not be rewritten, what led
# to test fail.
chattr -ai $LOG >/dev/null 2>&1
rm -f $LOG >/dev/null 2>&1

# make sure we stripe over all OSTs to avoid OOS on only a subset of OSTs
$LFS setstripe $OOS 65536 0 $STRIPECOUNT
if dd if=/dev/zero of=$OOS count=$(($ORIGFREE + 100)) bs=1k 2> $LOG; then
	echo "ERROR: dd did not fail"
	SUCCESS=0
fi

if [ "`grep -c 'No space left on device' $LOG`" -ne 1 ]; then
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
	grep [0-9] /proc/fs/lustre/osc/OSC*MNT*/{kbytesavail,cur*}
	SUCCESS=0
fi

RECORDSOUT=`grep "records out" $LOG | cut -d + -f1`

FILESIZE=`ls -l $OOS | awk '{ print $5 }'`
if [ $RECORDSOUT -ne $(($FILESIZE / 1024)) ]; then
        echo "ERROR: blocks written by dd not equal to the size of file"
        SUCCESS=0
fi

#lctl debug_daemon stop

rm -f $OOS

if [ $SUCCESS -eq 1 ]; then
	echo "Success!"
else
	exit 1
fi
