#!/bin/bash

export PATH=`dirname $0`/../utils:$PATH

config=${1:-local.xml}

LMC="${LMC:-lmc} -m $config"
TMP=${TMP:-/tmp}

FSTYPE=${FSTYPE:-ext3}

MDSDEV=${MDSDEV:-$TMP/mds1-`hostname`}
MDSSIZE=${MDSSIZE:-100000}

MOUNT=${MOUNT:-/mnt/lustre}
MOUNT2=${MOUNT2:-${MOUNT}2}
NETWORKTYPE=${NETWORKTYPE:-tcp}

OSTDEV=${OSTDEV:-$TMP/ost1-`hostname`}
OSTSIZE=${OSTSIZE:-200000}

MDS_BACKFSTYPE=${MDS_BACKFSTYPE:-ext3}
OST_BACKFSTYPE=${OST_BACKFSTYPE:-ext3}

MDS_BACKDEV=${MDS_BACKDEV:-$TMP/mds1-`hostname`}
OST_BACKDEV=${OST_BACKDEV:-$TMP/ost1-`hostname`}

# specific journal size for the ost, in MB
JSIZE=${JSIZE:-0}
[ "$JSIZE" -gt 0 ] && JARG="--journal_size $JSIZE"
MDSISIZE=${MDSISIZE:-0}
[ "$MDSISIZE" -gt 0 ] && IARG="--inode_size $MDSISIZE"

STRIPE_BYTES=65536
STRIPES_PER_OBJ=0	# 0 means stripe over all OSTs

rm -f $config

# create nodes
${LMC} --add node --node localhost || exit 10
${LMC} --add net --node  localhost --nid `hostname` --nettype $NETWORKTYPE || exit 11
${LMC} --add net --node client --nid '*' --nettype $NETWORKTYPE || exit 12

[ "x$MDS_MOUNT_OPTS" != "x" ] &&
    MDS_MOUNT_OPTS="--mountfsoptions $MDS_MOUNT_OPTS"

# configure mds server
${LMC} --add mds --nspath /mnt/mds_ns --node localhost --mds mds1 \
--fstype $FSTYPE --backfstype $MDS_BACKFSTYPE --dev $MDSDEV \
--backdev $MDS_BACKDEV $MDS_MOUNT_OPTS --size $MDSSIZE $JARG $IARG || exit 20

[ "x$OST_MOUNT_OPTS" != "x" ] &&
    OST_MOUNT_OPTS="--mountfsoptions $OST_MOUNT_OPTS"

# configure ost
${LMC} -m $config --add lov --lov lov1 --mds mds1 --stripe_sz $STRIPE_BYTES \
--stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0 || exit 20

${LMC} --add ost --nspath /mnt/ost_ns --node localhost --lov lov1 \
--fstype $FSTYPE --backfstype $OST_BACKFSTYPE --dev $OSTDEV \
--backdev $OST_BACKDEV $OST_MOUNT_OPTS --size $OSTSIZE $JARG || exit 30

# create client config
${LMC} --add mtpt --node localhost --path $MOUNT --mds mds1 --lov lov1 || exit 40
${LMC} --add mtpt --node client --path $MOUNT2 --mds mds1 --lov lov1 || exit 41
