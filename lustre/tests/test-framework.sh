#!/bin/bash

set -e

export REFORMAT=""
export VERBOSE=false
export GMNALNID=${GMNALNID:-/usr/sbin/gmlndnid}

# eg, assert_env LUSTRE MDSNODES OSTNODES CLIENTS
assert_env() {
    local failed=""
    for name in $@; do
      if [ -z "${!name}" ]; then
	  echo "$0: $name must be set"
          failed=1
      fi
    done
    [ $failed ] && exit 1 || true
}

usage() {
    echo "usage: $0 [-r] [-f cfgfile]"
    echo "       -r: reformat"

    exit
}

init_test_env() {
    export LUSTRE=`absolute_path $LUSTRE`
    export TESTSUITE=`basename $0 .sh`
    export XMLCONFIG=${XMLCONFIG:-${TESTSUITE}.xml}
    export LTESTDIR=${LTESTDIR:-$LUSTRE/../ltest}

    [ -d /r ] && export ROOT=${ROOT:-/r}
    export TMP=${TMP:-$ROOT/tmp}

    export PATH=:$PATH:$LUSTRE/utils:$LUSTRE/tests
    export LLMOUNT=${LLMOUNT:-"llmount"}
    export LCONF=${LCONF:-"lconf"}
    export LMC=${LMC:-"lmc"}
    export LCTL=${LCTL:-"$LUSTRE/utils/lctl"}
    export CHECKSTAT="${CHECKSTAT:-checkstat} "
    export FSYTPE=${FSTYPE:-"ext3"}

    # Paths on remote nodes, if different 
    export RLUSTRE=${RLUSTRE:-$LUSTRE}
    export RPWD=${RPWD:-$PWD}

    # command line
    
    while getopts "rvf:" opt $*; do 
	case $opt in
	    f) CONFIG=$OPTARG;;
	    r) REFORMAT=--reformat;;
	    v) VERBOSE=true;;
	    \?) usage;;
	esac
    done

    shift $((OPTIND - 1))
    ONLY=${ONLY:-$*}

    # save the name of the config file for the upcall
    echo "XMLCONFIG=$LUSTRE/tests/$XMLCONFIG"  > $LUSTRE/tests/XMLCONFIG
#    echo "CONFIG=`canonical_path $CONFIG`"  > $LUSTRE/tests/CONFIG
}

# Facet functions
start() {
    facet=$1
    shift
    active=`facet_active $facet`
    do_facet $facet $LCONF --select ${facet}_svc=${active}_facet \
        --node ${active}_facet  --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM \
        $@ $XMLCONFIG
}

stop() {
    facet=$1
    active=`facet_active $facet`
    shift
    do_facet $facet $LCONF --select ${facet}_svc=${active}_facet \
        --node ${active}_facet  --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM \
        $@ --cleanup $XMLCONFIG
}

zconf_mount() {
    local OPTIONS
    client=$1
    mnt=$2

    do_node $client mkdir $mnt 2> /dev/null || :

    # Only supply -o to mount if we have options
    if [ -n "$MOUNTOPT" ]; then
        OPTIONS="-o $MOUNTOPT"
    fi

    if [ -x /sbin/mount.lustre ] ; then
	do_node $client mount -t lustre $OPTIONS \
		`facet_nid mds`:/mds_svc/client_facet $mnt || return 1
    else
	# this is so cheating
	do_node $client $LCONF --nosetup --node client_facet $XMLCONFIG > \
		/dev/null || return 2
	do_node $client $LLMOUNT $OPTIONS \
		`facet_nid mds`:/mds_svc/client_facet $mnt || return 4
    fi

    [ -d /r ] && $LCTL modules > /r/tmp/ogdb-`hostname`
    return 0
}

zconf_umount() {
    client=$1
    mnt=$2
    [ "$3" ] && force=-f
    do_node $client umount $force  $mnt || :
    do_node $client $LCONF --cleanup --nosetup --node client_facet $XMLCONFIG > /dev/null || :
}

shutdown_facet() {
    facet=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_DOWN `facet_active_host $facet`
       sleep 2 
    elif [ "$FAILURE_MODE" = SOFT ]; then
       stop $facet --force --failover --nomod
    fi
}

reboot_facet() {
    facet=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       $POWER_UP `facet_active_host $facet`
    else
       sleep 10
    fi
}

wait_for_host() {
   HOST=$1
   check_network "$HOST" 900
   while ! do_node $HOST "ls -d $LUSTRE " > /dev/null; do sleep 5; done
}

wait_for() {
   facet=$1
   HOST=`facet_active_host $facet`
   wait_for_host $HOST
}

client_df() {
    # not every config has many clients
    if [ ! -z "$CLIENTS" ]; then
	$PDSH $CLIENTS "df $MOUNT" > /dev/null
    fi
}

client_reconnect() {
    uname -n >> $MOUNT/recon
    if [ ! -z "$CLIENTS" ]; then
	$PDSH $CLIENTS "df $MOUNT; uname -n >> $MOUNT/recon" > /dev/null
    fi
    echo Connected clients:
    cat $MOUNT/recon
    ls -l $MOUNT/recon > /dev/null
    rm $MOUNT/recon
}

facet_failover() {
    facet=$1
    echo "Failing $facet node `facet_active_host $facet`"
    shutdown_facet $facet
    reboot_facet $facet
    client_df &
    DFPID=$!
    echo "df pid is $DFPID"
    change_active $facet
    TO=`facet_active_host $facet`
    echo "Failover $facet to $TO"
    wait_for $facet
    start $facet
}

replay_barrier() {
    local facet=$1
    do_facet $facet sync
    df $MOUNT
    do_facet $facet $LCTL --device %${facet}_svc readonly
    do_facet $facet $LCTL --device %${facet}_svc notransno
    do_facet $facet $LCTL mark "$facet REPLAY BARRIER"
    $LCTL mark "local REPLAY BARRIER"
}

replay_barrier_nodf() {
    local facet=$1
    do_facet $facet sync
    do_facet $facet $LCTL --device %${facet}_svc readonly
    do_facet $facet $LCTL --device %${facet}_svc notransno
    do_facet $facet $LCTL mark "$facet REPLAY BARRIER"
    $LCTL mark "local REPLAY BARRIER"
}

mds_evict_client() {
    UUID=`cat /proc/fs/lustre/mdc/*_MNT_*/uuid`
    do_facet mds "echo $UUID > /proc/fs/lustre/mds/mds_svc/evict_client"
}

fail() {
    local facet=$1
    facet_failover $facet
    df $MOUNT || error "post-failover df: $?"
}

fail_abort() {
    local facet=$1
    stop $facet --force --failover --nomod
    change_active $facet
    start $facet
    do_facet $facet lctl --device %${facet}_svc abort_recovery
    df $MOUNT || echo "first df failed: $?"
    sleep 1
    df $MOUNT || error "post-failover df: $?"
}

do_lmc() {
    $LMC -m ${XMLCONFIG} $@
}

h2gm () {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
       ID=`$PDSH $1 $GMNALNID -l | cut -d\  -f2`
       echo $ID"@gm"
   fi
}

h2tcp() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   echo $1"@tcp" 
   fi
}
declare -fx h2tcp

h2elan() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   ID=`echo $1 | sed 's/[^0-9]*//g'`
   echo $ID"@elan"
   fi
}
declare -fx h2elan

h2openib() {
   if [ "$1" = "client" -o "$1" = "'*'" ]; then echo \'*\'; else
   ID=`echo $1 | sed 's/[^0-9]*//g'`
   echo $ID"@openib"
   fi
}
declare -fx h2openib

facet_host() {
   local facet=$1
   varname=${facet}_HOST
   echo -n ${!varname}
}

facet_nid() {
   facet=$1
   HOST=`facet_host $facet`
   if [ -z "$HOST" ]; then
	echo "The env variable ${facet}_HOST must be set."
	exit 1
   fi
   if [ -z "$NETTYPE" ]; then
	echo "The env variable NETTYPE must be set."
	exit 1
   fi
   echo `h2$NETTYPE $HOST`
}

facet_active() {
    local facet=$1
    local activevar=${facet}active

    if [ -f ./${facet}active ] ; then
        source ./${facet}active
    fi

    active=${!activevar}
    if [ -z "$active" ] ; then 
	echo -n ${facet}
    else
	echo -n ${active}
    fi
}

facet_active_host() {
    local facet=$1
    local active=`facet_active $facet`
    if [ "$facet" == client ]; then
	hostname
    else
	echo `facet_host $active`
    fi
}

change_active() {
    local facet=$1
    failover=${facet}failover 
    host=`facet_host $failover`
    [ -z "$host" ] && return
    curactive=`facet_active $facet`
    if [ -z "${curactive}" -o "$curactive" == "$failover" ] ; then
        eval export ${facet}active=$facet
    else
        eval export ${facet}active=$failover
    fi
    # save the active host for this facet
    activevar=${facet}active
    echo "$activevar=${!activevar}" > ./$activevar
}

do_node() {
    HOST=$1
    shift
    if $VERBOSE; then
        echo "CMD: $HOST $@"
        $PDSH $HOST $LCTL mark "$@" > /dev/null 2>&1 || :
    fi
    $PDSH $HOST "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; sh -c \"$@\")"
}

do_facet() {
    facet=$1
    shift
    HOST=`facet_active_host $facet`
    do_node $HOST $@
}

add_facet() {
    local facet=$1
    shift
    echo "add facet $facet: `facet_host $facet`"
    do_lmc --add node --node ${facet}_facet $@ --timeout $TIMEOUT \
        --lustre_upcall $UPCALL --ptldebug $PTLDEBUG --subsystem $SUBSYSTEM
    do_lmc --add net --node ${facet}_facet --nid `facet_nid $facet` --nettype lnet
}

add_mds() {
    local MOUNT_OPTS
    local facet=$1
    shift
    rm -f ${facet}active
    add_facet $facet
    [ "x$MDSOPT" != "x" ] && MOUNT_OPTS="--mountfsoptions $MDSOPT"
    do_lmc --add mds --node ${facet}_facet --mds ${facet}_svc \
    	--fstype $FSTYPE $* $MOUNT_OPTS
}

add_mdsfailover() {
    local MOUNT_OPTS
    local facet=$1
    shift
    add_facet ${facet}failover  --lustre_upcall $UPCALL
    [ "x$MDSOPT" != "x" ] && MOUNT_OPTS="--mountfsoptions $MDSOPT"
    do_lmc --add mds  --node ${facet}failover_facet --mds ${facet}_svc \
    	--fstype $FSTYPE $* $MOUNT_OPTS
}

add_ost() {
    facet=$1
    shift
    rm -f ${facet}active
    add_facet $facet
    do_lmc --add ost --node ${facet}_facet --ost ${facet}_svc \
    	--fstype $FSTYPE $* $OSTOPT
}

add_ostfailover() {
    facet=$1
    shift
    add_facet ${facet}failover
    do_lmc --add ost --failover --node ${facet}failover_facet \
    	--ost ${facet}_svc --fstype $FSTYPE $* $OSTOPT
}

add_lov() {
    lov=$1
    mds_facet=$2
    shift; shift
    do_lmc --add lov --mds ${mds_facet}_svc --lov $lov $* $LOVOPT
}

add_client() {
    local MOUNT_OPTS
    local facet=$1
    mds=$2
    shift; shift
    [ "x$CLIENTOPT" != "x" ] && MOUNT_OPTS="--clientoptions $CLIENTOPT"
    add_facet $facet --lustre_upcall $UPCALL
    do_lmc --add mtpt --node ${facet}_facet --mds ${mds}_svc $* $MOUNT_OPTS
}


####### 
# General functions

check_network() {
   local NETWORK=0
   local WAIT=0
   local MAX=$2
   while [ $NETWORK -eq 0 ]; do
      ping -c 1 -w 3 $1 > /dev/null
      if [ $? -eq 0 ]; then
         NETWORK=1
      else
         WAIT=$((WAIT + 5))
	 echo "waiting for $1, $((MAX - WAIT)) secs left"
         sleep 5
      fi
      if [ $WAIT -gt $MAX ]; then
         echo "Network not available"
         exit 1
      fi
   done
}
check_port() {
   while( !($DSH2 $1 "netstat -tna | grep -q $2") ) ; do
      sleep 9
   done
}

no_dsh() {
   shift
   eval $@
}

comma_list() {
    # the sed converts spaces to commas, but leaves the last space
    # alone, so the line doesn't end with a comma.
    echo "$*" | tr -s " " "\n" | sort -b -u | tr "\n" " " | sed 's/ \([^$]\)/,\1/g'
}

absolute_path() {
   (cd `dirname $1`; echo $PWD/`basename $1`)
}

##################################
# OBD_FAIL funcs

drop_request() {
# OBD_FAIL_MDS_ALL_REQUEST_NET
    RC=0
    do_facet mds sysctl -w lustre.fail_loc=0x123
    do_facet client "$1" || RC=$?
    do_facet mds sysctl -w lustre.fail_loc=0
    return $RC
}

drop_reply() {
# OBD_FAIL_MDS_ALL_REPLY_NET
    RC=0
    do_facet mds sysctl -w lustre.fail_loc=0x122
    do_facet client "$@" || RC=$?
    do_facet mds sysctl -w lustre.fail_loc=0
    return $RC
}

drop_reint_reply() {
# OBD_FAIL_MDS_REINT_NET_REP
    RC=0
    do_facet mds sysctl -w lustre.fail_loc=0x119
    do_facet client "$@" || RC=$?
    do_facet mds sysctl -w lustre.fail_loc=0
    return $RC
}

pause_bulk() {
#define OBD_FAIL_OST_BRW_PAUSE_BULK      0x214
    RC=0
    do_facet ost sysctl -w lustre.fail_loc=0x214
    do_facet client "$1" || RC=$?
    do_facet client "sync"
    do_facet ost sysctl -w lustre.fail_loc=0
    return $RC
}

drop_ldlm_cancel() {
#define OBD_FAIL_LDLM_CANCEL             0x304
    RC=0
    do_facet client sysctl -w lustre.fail_loc=0x304
    do_facet client "$@" || RC=$?
    do_facet client sysctl -w lustre.fail_loc=0
    return $RC
}

drop_bl_callback() {
#define OBD_FAIL_LDLM_BL_CALLBACK        0x305
    RC=0
    do_facet client sysctl -w lustre.fail_loc=0x305
    do_facet client "$@" || RC=$?
    do_facet client sysctl -w lustre.fail_loc=0
    return $RC
}

clear_failloc() {
    facet=$1
    pause=$2
    sleep $pause
    echo "clearing fail_loc on $facet"
    do_facet $facet "sysctl -w lustre.fail_loc=0"
}

cancel_lru_locks() {
    $LCTL mark "cancel_lru_locks start"
    for d in /proc/fs/lustre/ldlm/namespaces/$1*; do
	if [ -f $d/lru_size ]; then
	    echo clear > $d/lru_size
	    grep "[0-9]" $d/lock_unused_count
	fi
    done
    $LCTL mark "cancel_lru_locks stop"
}


pgcache_empty() {
    for a in /proc/fs/lustre/llite/*/dump_page_cache; do
        if [ `wc -l $a | awk '{print $1}'` -gt 1 ]; then
                echo there is still data in page cache $a ?
                cat $a;
                return 1;
        fi
    done
    return 0
}

##################################
# Test interface 
error() {
	sysctl -w lustre.fail_loc=0
	echo "${TESTSUITE}: **** FAIL:" $@
	log "FAIL: $@"
	exit 1
}

build_test_filter() {
        [ "$ONLY" ] && log "only running test `echo $ONLY`"
        for O in $ONLY; do
            eval ONLY_${O}=true
        done
        [ "$EXCEPT$ALWAYS_EXCEPT" ] && \
		log "skipping tests: `echo $EXCEPT $ALWAYS_EXCEPT`"
        for E in $EXCEPT $ALWAYS_EXCEPT; do
            eval EXCEPT_${E}=true
        done
}

_basetest() {
    echo $*
}

basetest() {
    IFS=abcdefghijklmnopqrstuvwxyz _basetest $1
}

run_test() {
        export base=`basetest $1`
        if [ ! -z "$ONLY" ]; then
                 testname=ONLY_$1
                 if [ ${!testname}x != x ]; then
                     run_one $1 "$2"
                     return $?
                 fi
                 testname=ONLY_$base
                 if [ ${!testname}x != x ]; then
                     run_one $1 "$2"
                     return $?
                 fi
                 echo -n "."
                 return 0
        fi
        testname=EXCEPT_$1
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1"
                 return 0
        fi
        testname=EXCEPT_$base
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1 (base $base)"
                 return 0
        fi
        run_one $1 "$2"

        return $?
}

EQUALS="======================================================================"
equals_msg() {
   msg="$@"

   local suffixlen=$((${#EQUALS} - ${#msg}))
   [ $suffixlen -lt 5 ] && suffixlen=5
   printf '===== %s %.*s\n' "$msg" $suffixlen $EQUALS
}

log() {
	echo "$*"
	$LCTL mark "$*" 2> /dev/null || true
}

pass() {
	echo PASS $@
}

check_mds() {
    FFREE=`cat /proc/fs/lustre/mds/*/filesfree`
    FTOTAL=`cat /proc/fs/lustre/mds/*/filestotal`
    [ $FFREE -ge $FTOTAL ] && error "files free $FFREE > total $FTOTAL" || true
}

run_one() {
    testnum=$1
    message=$2
    tfile=f${testnum}
    tdir=d${base}

    # Pretty tests run faster.
    equals_msg $testnum: $message

    BEFORE=`date +%s`
    log "== test $testnum: $message ============ `date +%H:%M:%S` ($BEFORE)"
    #check_mds
    test_${testnum} || error "test_$testnum failed with $?"
    #check_mds
    pass "($((`date +%s` - $BEFORE))s)"
}

canonical_path() {
   (cd `dirname $1`; echo $PWD/`basename $1`)
}

