#!/bin/sh
# Test multiple failures, AKA Test 17

set -e

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/insanity-local.sh}

ALWAYS_EXCEPT="10 $INSANITY_EXCEPT"

SETUP=${SETUP:-"setup"}
CLEANUP=${CLEANUP:-"cleanup"}

build_test_filter

assert_env mds_HOST MDS_MKFS_OPTS MDSDEV
assert_env ost_HOST OST_MKFS_OPTS OSTCOUNT
assert_env LIVE_CLIENT FSNAME


# This can be a regexp, to allow more clients
CLIENTS=${CLIENTS:-"`comma_list $LIVE_CLIENT $FAIL_CLIENTS $EXTRA_CLIENTS`"}

DIR=${DIR:-$MOUNT}

#####
# fail clients round robin

# list of failable clients
FAIL_LIST=($FAIL_CLIENTS)
FAIL_NUM=${#FAIL_LIST[*]}
FAIL_NEXT=0
typeset -i  FAIL_NEXT
DOWN_NUM=0   # number of nodes currently down

# set next client to fail
set_fail_client() {
    FAIL_CLIENT=${FAIL_LIST[$FAIL_NEXT]}
    FAIL_NEXT=$(( (FAIL_NEXT+1) % FAIL_NUM ))
    echo "fail $FAIL_CLIENT, next is $FAIL_NEXT"
}

shutdown_client() {
    client=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_DOWN $client
       while ping -w 3 -c 1 $client > /dev/null 2>&1; do 
	   echo "waiting for node $client to fail"
	   sleep 1
       done  
    elif [ "$FAILURE_MODE" = SOFT ]; then
       zconf_umount $client $MOUNT -f
    fi
}

reboot_node() {
    NODE=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_UP $NODE
    fi
}

fail_clients() {
    num=$1
    if [ -z "$num"  ] || [ "$num" -gt $((FAIL_NUM - DOWN_NUM)) ]; then
	num=$((FAIL_NUM - DOWN_NUM)) 
    fi
    
    if [ -z "$num" ] || [ "$num" -le 0 ]; then
        return
    fi

    client_mkdirs

    for i in `seq $num`; do
       set_fail_client
       client=$FAIL_CLIENT
       DOWN_CLIENTS="$DOWN_CLIENTS $client"
       shutdown_client $client
    done

    echo "down clients: $DOWN_CLIENTS"

    for client in $DOWN_CLIENTS; do
	reboot_node $client
    done
    DOWN_NUM=`echo $DOWN_CLIENTS | wc -w`
    client_rmdirs
}

reintegrate_clients() {
    for client in $DOWN_CLIENTS; do
	wait_for_host $client
	echo "Restarting $client"
	zconf_mount $client $MOUNT || return 1
    done
    DOWN_CLIENTS=""
    DOWN_NUM=0
}

start_ost() {
    start ost$1 `ostdevname $1` $OST_MOUNT_OPTS
}

setup() {
    cleanup
    rm -rf logs/*
    formatall
    setupall

    while ! do_node $CLIENTS "ls -d $LUSTRE" > /dev/null; do sleep 5; done
    grep " $MOUNT " /proc/mounts || zconf_mount $CLIENTS $MOUNT
}

cleanup() {
    zconf_umount $CLIENTS $MOUNT
    cleanupall
    cleanup_check
}

trap exit INT

client_touch() {
    file=$1
    for c in $LIVE_CLIENT $FAIL_CLIENTS;  do
	if echo $DOWN_CLIENTS | grep -q $c; then continue; fi
	$PDSH $c touch $MOUNT/${c}_$file || return 1
    done
}

client_rm() {
    file=$1
    for c in $LIVE_CLIENT $FAIL_CLIENTS;  do
	$PDSH $c rm $MOUNT/${c}_$file
    done
}

client_mkdirs() {
    for c in $LIVE_CLIENT $FAIL_CLIENTS;  do
	echo "$c mkdir $MOUNT/$c"
	$PDSH $c "mkdir $MOUNT/$c"
	$PDSH $c "ls -l $MOUNT/$c" 
    done
}

client_rmdirs() {
    for c in $LIVE_CLIENT $FAIL_CLIENTS;  do
	echo "rmdir $MOUNT/$c"
	$PDSH $LIVE_CLIENT "rmdir $MOUNT/$c"
    done
}

clients_recover_osts() {
    facet=$1
#    do_node $CLIENTS "$LCTL "'--device %OSC_`hostname`_'"${facet}_svc_MNT_client_facet recover"
}

if [ "$ONLY" == "cleanup" ]; then
    $CLEANUP
    exit
fi

if [ ! -z "$EVAL" ]; then
    eval "$EVAL"
    exit $?
fi

$SETUP

if [ "$ONLY" == "setup" ]; then
    exit 0
fi

# 9 Different Failure Modes Combinations
echo "Starting Test 17 at `date`"

test_0() {
    facet_failover mds
    echo "Waiting for df pid: $DFPID"
    wait $DFPID || { echo "df returned $?" && return 1; }

    facet_failover ost1 || return 4
    echo "Waiting for df pid: $DFPID"
    wait $DFPID || { echo "df returned $?" && return 2; }

    facet_failover ost2 || return 5
    echo "Waiting for df pid: $DFPID"
    wait $DFPID || { echo "df returned $?" && return 3; }
    return 0
}
run_test 0 "Fail all nodes, independently"

############### First Failure Mode ###############
test_1() {
echo "Don't do a MDS - MDS Failure Case"
echo "This makes no sense"
}
run_test 1 "MDS/MDS failure"
###################################################

############### Second Failure Mode ###############
test_2() {
    echo "Verify Lustre filesystem is up and running"
    client_df

    shutdown_facet mds
    reboot_facet mds

    # prepare for MDS failover
    change_active mds
    reboot_facet mds

    client_df &
    DFPID=$!
    sleep 5

    shutdown_facet ost1

    echo "Reintegrating OST"
    reboot_facet ost1
    wait_for ost1
    start_ost 1 || return 2

    wait_for mds
    start mds $MDSDEV $MDS_MOUNT_OPTS || return $?

    #Check FS
    wait $DFPID
    clients_recover_osts ost1
    echo "Verify reintegration"
    client_df || return 1

}
run_test 2 "Second Failure Mode: MDS/OST `date`"
###################################################


############### Third Failure Mode ###############
test_3() {
    #Create files
    echo "Verify Lustre filesystem is up and running"
    
    #MDS Portion
    facet_failover mds
    wait $DFPID || echo df failed: $?
    #Check FS

    echo "Test Lustre stability after MDS failover"
    client_df

    #CLIENT Portion
    echo "Failing 2 CLIENTS"
    fail_clients 2
    
    #Check FS
    echo "Test Lustre stability after CLIENT failure"
    client_df
    
    #Reintegration
    echo "Reintegrating CLIENTS"
    reintegrate_clients || return 1

    client_df || return 3
}
run_test 3  "Thirdb Failure Mode: MDS/CLIENT `date`"
###################################################

############### Fourth Failure Mode ###############
test_4() {
    echo "Fourth Failure Mode: OST/MDS `date`"

    #OST Portion
    shutdown_facet ost1
 
    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df &
    DFPIDA=$!
    sleep 5

    #MDS Portion
    shutdown_facet mds
    reboot_facet mds

    # prepare for MDS failover
    change_active mds
    reboot_facet mds

    client_df &
    DFPIDB=$!
    sleep 5

    #Reintegration
    echo "Reintegrating OST"
    reboot_facet ost1
    wait_for ost1
    start_ost 1
    
    wait_for mds
    start mds $MDSDEV $MDS_MOUNT_OPTS
    #Check FS
    
    wait $DFPIDA
    wait $DFPIDB
    clients_recover_osts ost1
    echo "Test Lustre stability after MDS failover"
    client_df || return 1
}
run_test 4 "Fourth Failure Mode: OST/MDS `date`"
###################################################

############### Fifth Failure Mode ###############
test_5() {
    echo "Fifth Failure Mode: OST/OST `date`"

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    
    #OST Portion
    shutdown_facet ost1
    reboot_facet ost1
    
    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df &
    DFPIDA=$!
    sleep 5
    
    #OST Portion
    shutdown_facet ost2
    reboot_facet ost2

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df &
    DFPIDB=$!
    sleep 5

    #Reintegration
    echo "Reintegrating OSTs"
    wait_for ost1
    start_ost 1
    wait_for ost2
    start_ost 2
    
    clients_recover_osts ost1
    clients_recover_osts ost2
    sleep $TIMEOUT

    wait $DFPIDA
    wait $DFPIDB
    client_df || return 2
}
run_test 5 "Fifth Failure Mode: OST/OST `date`"
###################################################

############### Sixth Failure Mode ###############
test_6() {
    echo "Sixth Failure Mode: OST/CLIENT `date`"

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df || return 1
    client_touch testfile || return 2
	
    #OST Portion
    shutdown_facet ost1
    reboot_facet ost1

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df &
    DFPIDA=$!
    sleep 5

    #CLIENT Portion
    echo "Failing CLIENTs"
    fail_clients
    
    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df &
    DFPIDB=$!
    sleep 5
    
    #Reintegration
    echo "Reintegrating OST/CLIENTs"
    wait_for ost1
    start_ost 1
    reintegrate_clients
    sleep 5 

    wait $DFPIDA
    wait $DFPIDB
    echo "Verifying mount"
    client_df || return 3
}
run_test 6 "Sixth Failure Mode: OST/CLIENT `date`"
###################################################


############### Seventh Failure Mode ###############
test_7() {
    echo "Seventh Failure Mode: CLIENT/MDS `date`"

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    client_touch testfile  || return 1

    #CLIENT Portion
    echo "Part 1: Failing CLIENT"
    fail_clients 2
    
    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df
    $PDSH $LIVE_CLIENT "ls -l $MOUNT"
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile"
    
    #Sleep
    echo "Wait 1 minutes"
    sleep 60

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    client_rm testfile

    #MDS Portion
    facet_failover mds

    #Check FS
    echo "Test Lustre stability after MDS failover"
    wait $DFPID || echo "df on down clients fails " || return 1
    $PDSH $LIVE_CLIENT "ls -l $MOUNT"
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile"

    #Reintegration
    echo "Reintegrating CLIENTs"
    reintegrate_clients
    client_df || return 2
    
    #Sleep
    echo "wait 1 minutes"
    sleep 60
}
run_test 7 "Seventh Failure Mode: CLIENT/MDS `date`"
###################################################


############### Eighth Failure Mode ###############
test_8() {
    echo "Eighth Failure Mode: CLIENT/OST `date`"

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    client_touch testfile
	
    #CLIENT Portion
    echo "Failing CLIENTs"
    fail_clients 2

    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df
    $PDSH $LIVE_CLIENT "ls -l $MOUNT"
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile"

    #Sleep
    echo "Wait 1 minutes"
    sleep 60

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    client_touch testfile


    #OST Portion
    shutdown_facet ost1
    reboot_facet ost1

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df &
    DFPID=$!
    sleep 5
    #non-failout hangs forever here
    #$PDSH $LIVE_CLIENT "ls -l $MOUNT"
    #$PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile"
    
    #Reintegration
    echo "Reintegrating CLIENTs/OST"
    reintegrate_clients
    wait_for ost1
    start_ost 1
    wait $DFPID
    client_df || return 1
    client_touch testfile2 || return 2

    #Sleep
    echo "Wait 1 minutes"
    sleep 60
}
run_test 8 "Eighth Failure Mode: CLIENT/OST `date`"
###################################################


############### Ninth Failure Mode ###############
test_9() {
    echo 

    #Create files
    echo "Verify Lustre filesystem is up and running"
    client_df
    client_touch testfile || return 1
	
    #CLIENT Portion
    echo "Failing CLIENTs"
    fail_clients 2

    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df
    $PDSH $LIVE_CLIENT "ls -l $MOUNT" || return 1
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile" || return 2

    #Sleep
    echo "Wait 1 minutes"
    sleep 60

    #Create files
    echo "Verify Lustre filesystem is up and running"
    $PDSH $LIVE_CLIENT df $MOUNT || return 3
    client_touch testfile || return 4

    #CLIENT Portion
    echo "Failing CLIENTs"
    fail_clients 2
    
    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df
    $PDSH $LIVE_CLIENT "ls -l $MOUNT" || return 5
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile" || return 6

    #Reintegration
    echo "Reintegrating  CLIENTs/CLIENTs"
    reintegrate_clients
    client_df || return 7
    
    #Sleep
    echo "Wait 1 minutes"
    sleep 60
}
run_test 9 "Ninth Failure Mode: CLIENT/CLIENT `date`"
###################################################

test_10() {
    #Run availability after all failures
    DURATION=${DURATION:-$((2 * 60 * 60))} # 6 hours default
    LOADTEST=${LOADTEST:-metadata-load.py}
    $PWD/availability.sh $CONFIG $DURATION $CLIENTS || return 1
}
run_test 10 "Running Availability for 6 hours..."

equals_msg "Done, cleaning up"
$CLEANUP
echo "$0: completed"

