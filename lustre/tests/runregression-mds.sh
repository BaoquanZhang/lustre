#!/bin/sh

SRCDIR="`dirname $0`"

ENDRUN=endrun-`hostname`

fail() { 
	echo "ERROR: $1" 1>&2
	[ $2 ] && RC=$2 || RC=1
	exit $RC
}

export PATH=/sbin:/usr/sbin:$SRCDIR:$PATH

cleanup() {
	trap 0
        $LCONF --cleanup $OPTS
}

[ "$COUNT" ] || COUNT=1000

[ "$LCONF" ] || LCONF=$SRCDIR/../utils/lconf

[ -z "$*" ] && fail "usage: $0 [--reformat] <conf>.xml" 1

OSCMT="`mount | awk '/ lustre_lite / { print $3 }' | tail -n 1`"
if [ -z "$OSCMT" ]; then
	$LCONF $@ || exit 1
        trap cleanup EXIT
	OSCMT="`mount | awk '/ lustre_lite / { print $3 }' | tail -n 1`"
	[ -z "$OSCMT" ] && fail "no lustre filesystem mounted" 1
fi

V="-10"
while [ "$1" ]; do
	case $1 in
	-v|--verbose) V="1";;
	--reformat) : ;;
	*) OPTS="$OPTS $1" ;;
	esac
	shift
done

OSCTMP=`echo $OSCMT | tr "/" "."`
USED=`df | awk "/$OSCTMP/ { print \\$3 }" | tail -n 1`
USED=`expr $USED + 16`	# Some space for the status file

THREADS=1
while [ $THREADS -lt 196 ]; do
	echo "starting $THREADS threads at `date`"
	[ $V -gt 0 ] || echo 0 > /proc/sys/lnet/debug
	$SRCDIR/createdestroy /mnt/lustre/file-$$ $COUNT $V $THREADS
	$SRCDIR/openclose /mnt/lustre/file-$$ $COUNT $THREADS
	THREADS=`expr $THREADS + 5`
	$LCONF --cleanup $OPTS || fail 10
	$LCONF $OPTS || fail 11
done

rm -f $ENDRUN

NOWUSED=`df | awk "/$OSCTMP/ { print \\$3 }" | tail -n 1`
if [ $NOWUSED -gt $USED ]; then
	echo "Space not all freed: now ${NOWUSED}kB, was ${USED}kB." 1>&2
	echo "This is normal on BA OSTs, because of subdirectories." 1>&2
fi

cleanup
