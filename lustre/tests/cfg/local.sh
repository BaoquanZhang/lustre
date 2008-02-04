FSNAME=${FSNAME:-lustre}

# facet hosts
mds_HOST=${mds_HOST:-`hostname`}
mdsfailover_HOST=${mdsfailover_HOST}
mds1_HOST=${mds1_HOST:-$mds_HOST}
mds1failover_HOST=${mds1failover_HOST:-$mdsfailover_HOST}
mgs_HOST=${mgs_HOST:-$mds_HOST}
ost_HOST=${ost_HOST:-`hostname`}
ostfailover_HOST=${ostfailover_HOST}

TMP=${TMP:-/tmp}

DAEMONSIZE=${DAEMONSIZE:-500}
MDSDEV=${MDSDEV:-$TMP/${FSNAME}-mdt1}
MDSCOUNT=${MDSCOUNT:-1}
MDSDEVBASE=${MDSDEVBASE:-$TMP/${FSNAME}-mdt}
MDSSIZE=${MDSSIZE:-100000}
MDSOPT=${MDSOPT:-"--mountfsoptions=acl"}

OSTCOUNT=${OSTCOUNT:-2}
OSTDEVBASE=${OSTDEVBASE:-$TMP/${FSNAME}-ost}
OSTSIZE=${OSTSIZE:-200000}
OSTOPT=""
# Can specify individual ost devs with
# OSTDEV1="/dev/sda"
# on specific hosts with
# ost1_HOST="uml2"

NETTYPE=${NETTYPE:-tcp}
MGSNID=${MGSNID:-`h2$NETTYPE $mgs_HOST`}
FSTYPE=${FSTYPE:-ldiskfs}
STRIPE_BYTES=${STRIPE_BYTES:-1048576}
STRIPES_PER_OBJ=${STRIPES_PER_OBJ:-0}
SINGLEMDS=${SINGLEMDS:-"mds1"}
TIMEOUT=${TIMEOUT:-20}
PTLDEBUG=${PTLDEBUG:-0x33f0404}
DEBUG_SIZE=${DEBUG_SIZE:-10}
SUBSYSTEM=${SUBSYSTEM:- 0xffb7e3ff}

MKFSOPT=""
MOUNTOPT=""
[ "x$MDSJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$MDSJOURNALSIZE"
[ "x$MDSISIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -i $MDSISIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\\\"$MKFSOPT\\\""
[ "x$MDSCAPA" != "x" ] &&
    MKFSOPT="--param mdt.capa=$MDSCAPA"
[ "x$mdsfailover_HOST" != "x" ] &&
    MOUNTOPT=$MOUNTOPT" --failnode=`h2$NETTYPE $mdsfailover_HOST`"
[ "x$STRIPE_BYTES" != "x" ] &&
    MOUNTOPT=$MOUNTOPT" --param lov.stripesize=$STRIPE_BYTES"
[ "x$STRIPES_PER_OBJ" != "x" ] &&
    MOUNTOPT=$MOUNTOPT" --param lov.stripecount=$STRIPES_PER_OBJ"
[ "x$L_GETIDENTITY" != "x" ] &&
    MOUNTOPT=$MOUNTOPT" --param mdt.identity_upcall=$L_GETIDENTITY"
MDS_MKFS_OPTS="--mgs --mdt --fsname=$FSNAME --device-size=$MDSSIZE --param sys.timeout=$TIMEOUT $MKFSOPT $MOUNTOPT $MDSOPT"

MKFSOPT=""
MOUNTOPT=""
[ "x$OSTJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$OSTJOURNALSIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\"$MKFSOPT\""
[ "x$OSSCAPA" != "x" ] &&
    MKFSOPT="--param ost.capa=$OSSCAPA"
[ "x$ostfailover_HOST" != "x" ] &&
    MOUNTOPT=$MOUNTOPT" --failnode=`h2$NETTYPE $ostfailover_HOST`"
OST_MKFS_OPTS="--ost --fsname=$FSNAME --device-size=$OSTSIZE --mgsnode=$MGSNID --param sys.timeout=$TIMEOUT $MKFSOPT $MOUNTOPT $OSTOPT"

MDS_MOUNT_OPTS=${MDS_MOUNT_OPTS:-"-o loop,user_xattr,acl"}
OST_MOUNT_OPTS=${OST_MOUNT_OPTS:-"-o loop"}

#client
MOUNT=${MOUNT:-/mnt/${FSNAME}}
MOUNT1=${MOUNT1:-$MOUNT}
MOUNT2=${MOUNT2:-${MOUNT}2}
MOUNTOPT=${MOUNTOPT:-"user_xattr,acl"}
[ "x$RMTCLIENT" != "x" ] &&
	MOUNTOPT=$MOUNTOPT",remote_client"
DIR=${DIR:-$MOUNT}
DIR1=${DIR:-$MOUNT1}
DIR2=${DIR2:-$MOUNT2}

PDSH=${PDSH:-no_dsh}
FAILURE_MODE=${FAILURE_MODE:-SOFT} # or HARD
POWER_DOWN=${POWER_DOWN:-"powerman --off"}
POWER_UP=${POWER_UP:-"powerman --on"}
