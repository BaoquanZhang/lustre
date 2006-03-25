#!/bin/sh

set -e

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/local.sh}

ostfailover_HOST=${ostfailover_HOST:-$ost_HOST}
#failover= must be defined in OST_MKFS_OPTIONS if ostfailover_HOST != ost_HOST

# Skip these tests
# BUG NUMBER: 2766?
ALWAYS_EXCEPT="5 $REPLAY_OST_SINGLE_EXCEPT"

gen_config() {
    grep " $MOUNT " /proc/mounts && zconf_umount `hostname` $MOUNT
    stop ost -f
    stop ost2 -f
    stop mds -f
    echo Formatting mds, ost
    add mds $MDS_MKFS_OPTS --reformat $MDSDEV
    add ost $OST_MKFS_OPTS --reformat $OSTDEV
}

cleanup() {
    # make sure we are using the primary server, so test-framework will
    # be able to clean up properly.
    activeost=`facet_active ost`
    if [ $activeost != "ost" ]; then
        fail ost
    fi

    zconf_umount `hostname` $MOUNT
    stop mds
    stop ost
    unload_modules
}

if [ "$ONLY" == "cleanup" ]; then
    sysctl -w lnet.debug=0
    cleanup
    exit
fi

build_test_filter

SETUP=${SETUP:-"setup"}
CLEANUP=${CLEANUP:-"cleanup"}

setup() {
    gen_config
    start mds $MDSDEV $MDS_MOUNT_OPTS
    start ost $OSTDEV $OST_MOUNT_OPTS
    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE

    if [ -z "`grep " $MOUNT " /proc/mounts`" ]; then
	# test "-1" needed during initial client->OST connection
	log "== test 00: target handle mismatch (bug 5317) === `date +%H:%M:%S`"
	#define OBD_FAIL_OST_ALL_REPLY_NET       0x211
	do_facet ost "sysctl -w lustre.fail_loc=0x80000211"
	zconf_mount `hostname` $MOUNT && df $MOUNT && pass || error "mount fail"
    fi
}

mkdir -p $DIR

$SETUP

test_0() {
    fail ost
    cp /etc/profile  $DIR/$tfile
    sync
    diff /etc/profile $DIR/$tfile
    rm -f $DIR/$tfile
}
run_test 0 "empty replay"

test_1() {
    date > $DIR/$tfile
    fail ost
    $CHECKSTAT -t file $DIR/$tfile || return 1
    rm -f $DIR/$tfile
}
run_test 1 "touch"

test_2() {
    for i in `seq 10`; do
        echo "tag-$i" > $DIR/$tfile-$i
    done 
    fail ost
    for i in `seq 10`; do
      grep -q "tag-$i" $DIR/$tfile-$i || error "f2-$i"
    done 
    rm -f $DIR/$tfile-*
}
run_test 2 "|x| 10 open(O_CREAT)s"

test_3() {
    verify=$ROOT/tmp/verify-$$
    dd if=/dev/urandom bs=4096 count=1280 | tee $verify > $DIR/$tfile &
    ddpid=$!
    sync &
    fail ost
    wait $ddpid || return 1
    cmp $verify $DIR/$tfile || return 2
    rm -f $verify $DIR/$tfile
}
run_test 3 "Fail OST during write, with verification"

test_4() {
    verify=$ROOT/tmp/verify-$$
    dd if=/dev/urandom bs=4096 count=1280 | tee $verify > $DIR/$tfile
    # invalidate cache, so that we're reading over the wire
    for i in /proc/fs/lustre/ldlm/namespaces/*-osc-*; do
        echo -n clear > $i/lru_size
    done
    cmp $verify $DIR/$tfile &
    cmppid=$!
    fail ost
    wait $cmppid || return 1
    rm -f $verify $DIR/$tfile
}
run_test 4 "Fail OST during read, with verification"

test_5() {
    FREE=`df -P -h $DIR | tail -n 1 | awk '{ print $3 }'`
    case $FREE in
    *T|*G) FREE=1G;;
    esac
    IOZONE_OPTS="-i 0 -i 1 -i 2 -+d -r 4 -s $FREE"
    iozone $IOZONE_OPTS -f $DIR/$tfile &
    PID=$!
    
    sleep 8
    fail ost
    wait $PID || return 1
    rm -f $DIR/$tfile
}
run_test 5 "Fail OST during iozone"

kbytesfree() {
   awk '{total+=$1} END {print total}' /proc/fs/lustre/osc/*-osc-*/kbytesfree
}

test_6() {
    f=$DIR/$tfile
    rm -f $f
    sync && sleep 2 && sync	# wait for delete thread
    before=`kbytesfree`
    dd if=/dev/urandom bs=4096 count=1280 of=$f
    lfs getstripe $f
#define OBD_FAIL_MDS_REINT_NET_REP       0x119
    do_facet mds "sysctl -w lustre.fail_loc=0x80000119"
    sync
    sleep 1					# ensure we have a fresh statfs
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    rm -f $f
    fail ost
    $CHECKSTAT -t file $f && return 2 || true
    sync
    # let the delete happen
    sleep 5
    after=`kbytesfree`
    log "before: $before after: $after"
    (( $before <= $after + 40 )) || return 3	# take OST logs into account
}
run_test 6 "Fail OST before obd_destroy"

test_7() {
    f=$DIR/$tfile
    rm -f $f
    sync && sleep 2 && sync	# wait for delete thread
    before=`kbytesfree`
    dd if=/dev/urandom bs=4096 count=1280 of=$f
    sync
    sleep 1					# ensure we have a fresh statfs
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    replay_barrier ost
    rm -f $f
    fail ost
    $CHECKSTAT -t file $f && return 2 || true
    sync
    # let the delete happen
    sleep 2
    after=`kbytesfree`
    log "before: $before after: $after"
    (( $before <= $after + 40 )) || return 3	# take OST logs into account
}
run_test 7 "Fail OST before obd_destroy"

equals_msg test complete, cleaning up
$CLEANUP
