#!/bin/bash

set -e

PTLDEBUG=${PTLDEBUG:--1}
LUSTRE=${LUSTRE:-`dirname $0`/..}
CLEANUP=${CLEANUP:-""}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

ostfailover_HOST=${ostfailover_HOST:-$ost_HOST}
#failover= must be defined in OST_MKFS_OPTIONS if ostfailover_HOST != ost_HOST

# Tests that fail on uml
CPU=`awk '/model/ {print $4}' /proc/cpuinfo`
[ "$CPU" = "UML" ] && EXCEPT="$EXCEPT 6"

# Skip these tests
# BUG NUMBER: 
ALWAYS_EXCEPT="$REPLAY_OST_SINGLE_EXCEPT"

# It is replay-ost-single, after all
OSTCOUNT=1

gen_config() {
    formatall
}

cleanup() {
    cleanupall
}

if [ "$ONLY" == "cleanup" ]; then
    cleanup
    exit
fi

build_test_filter

SETUP=${SETUP:-"setup"}

test_0a() {
    # needs to run during initial client->OST connection
    #define OBD_FAIL_OST_ALL_REPLY_NET       0x211
    do_facet ost "sysctl -w lustre.fail_loc=0x80000211"
    zconf_mount `hostname` $MOUNT && df $MOUNT || error "0a mount fail"
}

setup() {
    gen_config
    start mds1 `mdsdevname 1` $MDS_MOUNT_OPTS
    start ost1 `ostdevname 1` $OST_MOUNT_OPTS
    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE

    # this might not mount if we aren't running test 0a
    [ -z "`grep " $MOUNT " /proc/mounts`" ] && \
	run_test 0a "target handle mismatch (bug 5317) `date +%H:%M:%S`" 

    if [ -z "`grep " $MOUNT " /proc/mounts`" ]; then
	zconf_mount `hostname` $MOUNT || error "mount fail"
    fi
    sleep 5

    do_facet ost1 "sysctl -w lustre.fail_loc=0"
}

mkdir -p $DIR

$SETUP

test_0b() {
    fail ost1
    cp /etc/profile  $DIR/$tfile
    sync
    diff /etc/profile $DIR/$tfile
    rm -f $DIR/$tfile
}
run_test 0b "empty replay"

test_1() {
    date > $DIR/$tfile
    fail ost1
    $CHECKSTAT -t file $DIR/$tfile || return 1
    rm -f $DIR/$tfile
}
run_test 1 "touch"

test_2() {
    for i in `seq 10`; do
        echo "tag-$i" > $DIR/$tfile-$i
    done 
    fail ost1
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
    fail ost1
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
    fail ost1
    wait $cmppid || return 1
    rm -f $verify $DIR/$tfile
}
run_test 4 "Fail OST during read, with verification"

test_5() {
    [ -z "`which iozone 2> /dev/null`" ] && log "iozone missing" && return
    FREE=`df -P -h $DIR | tail -n 1 | awk '{ print $3 }'`
    case $FREE in
    *T|*G) FREE=1G;;
    esac
    IOZONE_OPTS="-i 0 -i 1 -i 2 -+d -r 4 -s $FREE"
    iozone $IOZONE_OPTS -f $DIR/$tfile &
    PID=$!
    
    sleep 8
    fail ost1
    wait $PID
    RC=$?
    log "iozone rc=$RC"
    rm -f $DIR/$tfile
    [ $RC -ne 0 ] && return $RC || true
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
    dd if=/dev/urandom bs=4096 count=1280 of=$f || return 28
    lfs getstripe $f
    sync
    sleep 2					# ensure we have a fresh statfs
    sync
#define OBD_FAIL_MDS_REINT_NET_REP       0x119
    do_facet mds "sysctl -w lustre.fail_loc=0x80000119"
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    rm -f $f
    fail ost1
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
    sync && sleep 5 && sync	# wait for delete thread
    before=`kbytesfree`
    dd if=/dev/urandom bs=4096 count=1280 of=$f || return 4
    sync
    sleep 2					# ensure we have a fresh statfs
    sync
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    replay_barrier ost1
    rm -f $f
    fail ost1
    $CHECKSTAT -t file $f && return 2 || true
    sync
    # let the delete happen
    sleep 5
    after=`kbytesfree`
    log "before: $before after: $after"
    (( $before <= $after + 40 )) || return 3	# take OST logs into account
}
run_test 7 "Fail OST before obd_destroy"

equals_msg `basename $0`: test complete, cleaning up
check_and_cleanup_lustre
[ -f "$TESTSUITELOG" ] && cat $TESTSUITELOG || true
