#!/bin/sh
# Test multiple failures, AKA Test 17

set -e

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh

init_test_env $@

. ${CONFIG:=$LUSTRE/tests/cfg/insanity-lmv.sh}

ALWAYS_EXCEPT="10"

SETUP=${SETUP:-"setup"}
CLEANUP=${CLEANUP:-"cleanup"}

build_test_filter

assert_env MDSCOUNT mds1_HOST ost1_HOST ost2_HOST client_HOST LIVE_CLIENT 

####
# Initialize all the ostN_HOST 
NUMOST=2
if [ "$EXTRA_OSTS" ]; then
    for host in $EXTRA_OSTS; do
	NUMOST=$((NUMOST + 1))
	OST=ost$NUMOST
	eval ${OST}_HOST=$host
    done
fi

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

gen_config() {
    rm -f $XMLCONFIG
    if [ "$MDSCOUNT" -gt 1 ]; then
        add_lmv lmv1_svc
        for mds in `mds_list`; do
            MDSDEV=$TMP/${mds}-`hostname`
            add_mds $mds --dev $MDSDEV --size $MDSSIZE --lmv lmv1_svc
        done
        add_lov_to_lmv lov1 lmv1_svc --stripe_sz $STRIPE_BYTES \
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	MDS=lmv1
    else
        add_mds mds1 --dev $MDSDEV --size $MDSSIZE
        if [ ! -z "$mds1failover_HOST" ]; then
	     add_mdsfailover mds1 --dev $MDSDEV --size $MDSSIZE
        fi
	add_lov lov1 mds1 --stripe_sz $STRIPE_BYTES \
	    --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
	MDS=mds1
    fi

    for i in `seq $NUMOST`; do
	dev=`printf $OSTDEV $i`
	add_ost ost$i --lov lov1 --dev $dev --size $OSTSIZE \
	    --journal-size $OSTJOURNALSIZE
    done
     
    add_client client $MDS --lov lov1 --path $MOUNT
}

setup() {
    gen_config

    rm -rf logs/*
    for i in `seq $NUMOST`; do
	wait_for ost$i
	start ost$i ${REFORMAT} $OSTLCONFARGS 
    done
    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE
    for mds in `mds_list`; do
	wait_for $mds
	start $mds $MDSLCONFARGS ${REFORMAT}
    done
    while ! do_node $CLIENTS "ls -d $LUSTRE" > /dev/null; do sleep 5; done
    grep " $MOUNT " /proc/mounts || zconf_mount $CLIENTS $MOUNT

}

cleanup() {
    zconf_umount $CLIENTS $MOUNT

    for mds in `mds_list`; do
	stop $mds ${FORCE} $MDSLCONFARGS || :
    done
    for i in `seq $NUMOST`; do
	stop ost$i ${REFORMAT} ${FORCE} $OSTLCONFARGS  || :
    done
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

node_to_ost() {
    node=$1
    retvar=$2
    for i in `seq $NUMOST`; do
	ostvar="ost${i}_HOST"
	if [ "${!ostvar}" == $node ]; then
	    eval $retvar=ost${i}
	    return 0
	fi
    done
    echo "No ost found for node; $node"
    return 1
    
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
    echo "Failover MDS"
    facet_failover mds1
    echo "Waiting for df pid: $DFPID"
    wait $DFPID || { echo "df returned $?" && return 1; }

    echo "Failing OST1"
    facet_failover ost1
    echo "Waiting for df pid: $DFPID"
    wait $DFPID || { echo "df returned $?" && return 2; }
    
    echo "Failing OST2"
    facet_failover ost2
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

    echo "Failing MDS"
    shutdown_facet mds1
    reboot_facet mds1

    # prepare for MDS failover
    change_active mds1
    reboot_facet mds1

    client_df &
    DFPID=$!
    sleep 5

    echo "Failing OST"
    shutdown_facet ost1

    echo "Reintegrating OST"
    reboot_facet ost1
    wait_for ost1
    start ost1

    echo "Failover MDS"
    wait_for mds1
    start mds1

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
    facet_failover mds1
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
    echo "Failing OST ost1"
    shutdown_facet ost1
 
    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df

    #MDS Portion
    echo "Failing MDS"
    shutdown_facet mds1
    reboot_facet mds1

    # prepare for MDS failover
    change_active mds1
    reboot_facet mds1

    client_df &
    DFPID=$!
    sleep 5

    #Reintegration
    echo "Reintegrating OST"
    reboot_facet ost1
    wait_for ost1
    start ost1
    
    echo "Failover MDS"
    wait_for mds1
    start mds1
    #Check FS
    
    wait $DFPID
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
    echo "Failing OST"
    shutdown_facet ost1
    reboot_facet ost1
    
    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df
    
    #OST Portion
    echo "Failing OST"
    shutdown_facet ost2
    reboot_facet ost2

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df

    #Reintegration
    echo "Reintegrating OSTs"
    wait_for ost1
    start ost1
    wait_for ost2
    start ost2
    
    clients_recover_osts ost1
    clients_recover_osts ost2
    sleep $TIMEOUT

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
    echo "Failing OST"
    shutdown_facet ost1
    reboot_facet ost1

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df

    #CLIENT Portion
    echo "Failing CLIENTs"
    fail_clients
    
    #Check FS
    echo "Test Lustre stability after CLIENTs failure"
    client_df
    
    #Reintegration
    echo "Reintegrating OST/CLIENTs"
    wait_for ost1
    start ost1
    reintegrate_clients
    sleep 5 

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
    echo "Failing MDS"
    facet_failover mds1

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
    echo "Failing OST"
    shutdown_facet ost1
    reboot_facet ost1

    #Check FS
    echo "Test Lustre stability after OST failure"
    client_df
    $PDSH $LIVE_CLIENT "ls -l $MOUNT"
    $PDSH $LIVE_CLIENT "rm -f $MOUNT/*_testfile"
    
    #Reintegration
    echo "Reintegrating CLIENTs/OST"
    reintegrate_clients
    wait_for ost1
    start ost1
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
