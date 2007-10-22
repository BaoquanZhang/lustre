#!/bin/bash
# requirement:
#	add uml1 uml2 uml3 in your /etc/hosts

# FIXME - there is no reason to use all of these different
#   return codes, espcially when most of them are mapped to something
#   else anyway.  The combination of test number and return code
#   figure out what failed.

set -e

ONLY=${ONLY:-"$*"}

# These tests don't apply to mountconf
#              xml xml xml xml xml xml dumb
MOUNTCONFSKIP="10  11  12  13  13b 14  15 "

# bug number for skipped test:                     13369 12743
ALWAYS_EXCEPT=" $CONF_SANITY_EXCEPT $MOUNTCONFSKIP 34a   36"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

SRCDIR=`dirname $0`
PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH

SAVE_PWD=$PWD
LUSTRE=${LUSTRE:-`dirname $0`/..}
RLUSTRE=${RLUSTRE:-$LUSTRE}
HOSTNAME=`hostname`

. $LUSTRE/tests/test-framework.sh
init_test_env $@
# use small MDS + OST size to speed formatting time
MDSSIZE=40000
OSTSIZE=40000
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

reformat() {
        formatall
}

writeconf() {
    local facet=mds
    shift
    stop ${facet} -f
    rm -f ${facet}active
    # who knows if/where $TUNEFS is installed?  Better reformat if it fails...
    do_facet ${facet} "$TUNEFS --writeconf $MDSDEV" || echo "tunefs failed, reformatting instead" && reformat
}

gen_config() {
        reformat
        # The MGS must be started before the OSTs for a new fs, so start
        # and stop to generate the startup logs. 
	start_mds
	start_ost
	sleep 5
	stop_ost
	stop_mds
}

start_mds() {
	echo "start mds service on `facet_active_host mds`"
	start mds $MDSDEV $MDS_MOUNT_OPTS || return 94
}

stop_mds() {
	echo "stop mds service on `facet_active_host mds`"
	# These tests all use non-failover stop
	stop mds -f  || return 97
}

start_ost() {
	echo "start ost1 service on `facet_active_host ost1`"
	start ost1 `ostdevname 1` $OST_MOUNT_OPTS || return 95
}

stop_ost() {
	echo "stop ost1 service on `facet_active_host ost1`"
	# These tests all use non-failover stop
	stop ost1 -f  || return 98
}

start_ost2() {
	echo "start ost2 service on `facet_active_host ost2`"
	start ost2 `ostdevname 2` $OST_MOUNT_OPTS || return 92
}

stop_ost2() {
	echo "stop ost2 service on `facet_active_host ost2`"
	# These tests all use non-failover stop
	stop ost2 -f  || return 93
}

start_client() {
	echo "start client on `facet_active_host client`"
	start client || return 99 
}

stop_client() {
	echo "stop client on `facet_active_host client`"
	stop client || return 100 
}

mount_client() {
	local MOUNTPATH=$1
	echo "mount $FSNAME on ${MOUNTPATH}....."
	zconf_mount `hostname` $MOUNTPATH  || return 96
}

remount_client() {
	local SAVEMOUNTOPT=$MOUNTOPT
	MOUNTOPT="remount,$1"
	local MOUNTPATH=$2
	echo "remount '$1' lustre on ${MOUNTPATH}....."
	zconf_mount `hostname`  $MOUNTPATH  || return 96
	MOUNTOPT=$SAVEMOUNTOPT
}

umount_client() {
	local MOUNTPATH=$1
	echo "umount lustre on ${MOUNTPATH}....."
	zconf_umount `hostname` $MOUNTPATH || return 97
}

manual_umount_client(){
	local rc
	local FORCE=$1
	echo "manual umount lustre on ${MOUNT}...."
	do_facet client "umount -d ${FORCE} $MOUNT"
	rc=$?
	return $rc
}

setup() {
	start_ost
	start_mds
	mount_client $MOUNT
}

cleanup_nocli() {
	stop_mds || return 201
	stop_ost || return 202
	unload_modules || return 203
}

cleanup() {
 	umount_client $MOUNT || return 200
	cleanup_nocli || return $?
}

check_mount() {
	do_facet client "cp /etc/passwd $DIR/a" || return 71
	do_facet client "rm $DIR/a" || return 72
	# make sure lustre is actually mounted (touch will block, 
        # but grep won't, so do it after) 
        do_facet client "grep $MOUNT' ' /proc/mounts > /dev/null" || return 73
	echo "setup single mount lustre success"
}

check_mount2() {
	do_facet client "touch $DIR/a" || return 71	
	do_facet client "rm $DIR/a" || return 72	
	do_facet client "touch $DIR2/a" || return 73	
	do_facet client "rm $DIR2/a" || return 74	
	echo "setup double mount lustre success"
}

build_test_filter

if [ "$ONLY" == "setup" ]; then
	setup
	exit
fi

if [ "$ONLY" == "cleanup" ]; then
	cleanup
	exit
fi

#create single point mountpoint

gen_config


test_0() {
        setup
	check_mount || return 41
	cleanup || return $?
}
run_test 0 "single mount setup"

test_1() {
	start_ost
	echo "start ost second time..."
	setup
	check_mount || return 42
	cleanup || return $?
}
run_test 1 "start up ost twice (should return errors)"

test_2() {
	start_ost
	start_mds	
	echo "start mds second time.."
	start_mds
	mount_client $MOUNT
	check_mount || return 43
	cleanup || return $?
}
run_test 2 "start up mds twice (should return err)"

test_3() {
	setup
	#mount.lustre returns an error if already in mtab
	mount_client $MOUNT && return $?
	check_mount || return 44
	cleanup || return $?
}
run_test 3 "mount client twice (should return err)"

test_4() {
	setup
	touch $DIR/$tfile || return 85
	stop_ost -f
	cleanup
	eno=$?
	# ok for ost to fail shutdown
	if [ 202 -ne $eno ]; then
		return $eno;
	fi
	return 0
}
run_test 4 "force cleanup ost, then cleanup"

test_5() {
	setup
	touch $DIR/$tfile || return 1
	fuser -m -v $MOUNT && echo "$MOUNT is in use by user space process."

	stop_mds -f || return 2

	# cleanup may return an error from the failed
	# disconnects; for now I'll consider this successful
	# if all the modules have unloaded.
 	umount -d $MOUNT &
	UMOUNT_PID=$!
	sleep 6
	echo "killing umount"
	kill -TERM $UMOUNT_PID
	echo "waiting for umount to finish"
	wait $UMOUNT_PID
	if grep " $MOUNT " /proc/mounts; then
		echo "test 5: /proc/mounts after failed umount"
		umount $MOUNT &
		UMOUNT_PID=$!
		sleep 2
		echo "killing umount"
		kill -TERM $UMOUNT_PID
		echo "waiting for umount to finish"
		wait $UMOUNT_PID
		grep " $MOUNT " /proc/mounts && echo "test 5: /proc/mounts after second umount" && return 11
	fi

	manual_umount_client
	# stop_mds is a no-op here, and should not fail
	cleanup_nocli || return $?
	# df may have lingering entry
	manual_umount_client
	# mtab may have lingering entry
	grep -v $MOUNT" " /etc/mtab > $TMP/mtabtemp
	mv $TMP/mtabtemp /etc/mtab
}
run_test 5 "force cleanup mds, then cleanup"

test_5b() {
	start_ost
	[ -d $MOUNT ] || mkdir -p $MOUNT
	grep " $MOUNT " /etc/mtab && echo "test 5b: mtab before mount" && return 10
	mount_client $MOUNT && return 1
	grep " $MOUNT " /etc/mtab && echo "test 5b: mtab after failed mount" && return 11
	umount_client $MOUNT	
	# stop_mds is a no-op here, and should not fail
	cleanup_nocli || return $?
	return 0
}
run_test 5b "mds down, cleanup after failed mount (bug 2712) (should return errs)"

test_5c() {
	start_ost
	start_mds
	[ -d $MOUNT ] || mkdir -p $MOUNT
	grep " $MOUNT " /etc/mtab && echo "test 5c: mtab before mount" && return 10
	mount -t lustre $MGSNID:/wrong.$FSNAME $MOUNT || :
	grep " $MOUNT " /etc/mtab && echo "test 5c: mtab after failed mount" && return 11
	umount_client $MOUNT
	cleanup_nocli  || return $?
}
run_test 5c "cleanup after failed mount (bug 2712) (should return errs)"

test_5d() {
	start_ost
	start_mds
	stop_ost -f
	grep " $MOUNT " /etc/mtab && echo "test 5d: mtab before mount" && return 10
	mount_client $MOUNT || return 1
	cleanup  || return $?
	grep " $MOUNT " /etc/mtab && echo "test 5d: mtab after unmount" && return 11
	return 0
}
run_test 5d "mount with ost down"

test_5e() {
	start_ost
	start_mds

#define OBD_FAIL_PTLRPC_DELAY_SEND       0x506
	do_facet client "sysctl -w lustre.fail_loc=0x80000506"
	grep " $MOUNT " /etc/mtab && echo "test 5e: mtab before mount" && return 10
	mount_client $MOUNT || echo "mount failed (not fatal)"
	cleanup  || return $?
	grep " $MOUNT " /etc/mtab && echo "test 5e: mtab after unmount" && return 11
	return 0
}
run_test 5e "delayed connect, don't crash (bug 10268)"

test_6() {
	setup
	manual_umount_client
	mount_client ${MOUNT} || return 87
	touch $DIR/a || return 86
	cleanup  || return $?
}
run_test 6 "manual umount, then mount again"

test_7() {
	setup
	manual_umount_client
	cleanup_nocli || return $?
}
run_test 7 "manual umount, then cleanup"

test_8() {
	setup
	mount_client $MOUNT2
	check_mount2 || return 45
	umount_client $MOUNT2
	cleanup  || return $?
}
run_test 8 "double mount setup"

test_9() {
        start_ost

	do_facet ost1 sysctl lnet.debug=\'inode trace\' || return 1
	do_facet ost1 sysctl lnet.subsystem_debug=\'mds ost\' || return 1

        CHECK_PTLDEBUG="`do_facet ost1 sysctl -n lnet.debug`"
        if [ "$CHECK_PTLDEBUG" ] && [ "$CHECK_PTLDEBUG" = "trace inode" ];then
           echo "lnet.debug success"
        else
           echo "lnet.debug: want 'trace inode', have '$CHECK_PTLDEBUG'"
           return 1
        fi
        CHECK_SUBSYS="`do_facet ost1 sysctl -n lnet.subsystem_debug`"
        if [ "$CHECK_SUBSYS" ] && [ "$CHECK_SUBSYS" = "mds ost" ]; then
           echo "lnet.subsystem_debug success"
        else
           echo "lnet.subsystem_debug: want 'mds ost', have '$CHECK_SUBSYS'"
           return 1
        fi
        stop_ost || return $?
}

run_test 9 "test ptldebug and subsystem for mkfs"

test_10() {
        echo "generate configuration with the same name for node and mds"
        OLDXMLCONFIG=$XMLCONFIG
        XMLCONFIG="broken.xml"
        [ -f "$XMLCONFIG" ] && rm -f $XMLCONFIG
        facet="mds"
        rm -f ${facet}active
        add_facet $facet
        echo "the name for node and mds is the same"
        do_lmc --add mds --node ${facet}_facet --mds ${facet}_facet \
            --dev $MDSDEV --size $MDSSIZE || return $?
        do_lmc --add lov --mds ${facet}_facet --lov lov1 --stripe_sz \
            $STRIPE_BYTES --stripe_cnt $STRIPES_PER_OBJ \
            --stripe_pattern 0 || return $?
        add_ost ost --lov lov1 --dev $OSTDEV --size $OSTSIZE
        facet="client"
        add_facet $facet --lustre_upcall $UPCALL
        do_lmc --add mtpt --node ${facet}_facet --mds mds_facet \
            --lov lov1 --path $MOUNT

        echo "mount lustre"
        start_ost
        start_mds
        mount_client $MOUNT
        check_mount || return 41
        cleanup || return $?

        echo "Success!"
        XMLCONFIG=$OLDXMLCONFIG
}
run_test 10 "mount lustre with the same name for node and mds"

test_11() {
        OLDXMLCONFIG=$XMLCONFIG
        XMLCONFIG="conf11.xml"

        [ -f "$XMLCONFIG" ] && rm -f $XMLCONFIG
        add_mds mds --dev $MDSDEV --size $MDSSIZE
        add_ost ost --dev $OSTDEV --size $OSTSIZE
        add_client client mds --path $MOUNT --ost ost_svc || return $?
        echo "Default lov config success!"

        [ -f "$XMLCONFIG" ] && rm -f $XMLCONFIG
        add_mds mds --dev $MDSDEV --size $MDSSIZE
        add_ost ost --dev $OSTDEV --size $OSTSIZE
        add_client client mds --path $MOUNT && return $?
        echo "--add mtpt with neither --lov nor --ost will return error"

        echo ""
        echo "Success!"
        XMLCONFIG=$OLDXMLCONFIG
}
run_test 11 "use default lov configuration (should return error)"

test_12() {
        OLDXMLCONFIG=$XMLCONFIG
        XMLCONFIG="batch.xml"
        BATCHFILE="batchfile"

        # test double quote
        [ -f "$XMLCONFIG" ] && rm -f $XMLCONFIG
        [ -f "$BATCHFILE" ] && rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions \"-I 128\"" >> $BATCHFILE
        # --mkfsoptions "-I 128"
        do_lmc -m $XMLCONFIG --batch $BATCHFILE || return $?
        if [ `sed -n '/>-I 128</p' $XMLCONFIG | wc -l` -eq 1 ]; then
                echo "matched double quote success"
        else
                echo "matched double quote fail"
                return 1
        fi
        rm -f $XMLCONFIG
        rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions \"-I 128" >> $BATCHFILE
        # --mkfsoptions "-I 128
        do_lmc -m $XMLCONFIG --batch $BATCHFILE && return $?
        echo "unmatched double quote should return error"

        # test single quote
        rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions '-I 128'" >> $BATCHFILE
        # --mkfsoptions '-I 128'
        do_lmc -m $XMLCONFIG --batch $BATCHFILE || return $?
        if [ `sed -n '/>-I 128</p' $XMLCONFIG | wc -l` -eq 1 ]; then
                echo "matched single quote success"
        else
                echo "matched single quote fail"
                return 1
        fi
        rm -f $XMLCONFIG
        rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions '-I 128" >> $BATCHFILE
        # --mkfsoptions '-I 128
        do_lmc -m $XMLCONFIG --batch $BATCHFILE && return $?
        echo "unmatched single quote should return error"

        # test backslash
        rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions \-\I\ \128" >> $BATCHFILE
        # --mkfsoptions \-\I\ \128
        do_lmc -m $XMLCONFIG --batch $BATCHFILE || return $?
        if [ `sed -n '/>-I 128</p' $XMLCONFIG | wc -l` -eq 1 ]; then
                echo "backslash followed by a whitespace/letter success"
        else
                echo "backslash followed by a whitespace/letter fail"
                return 1
        fi
        rm -f $XMLCONFIG
        rm -f $BATCHFILE
        echo "--add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp" > $BATCHFILE
        echo "--add mds --node $HOSTNAME --mds mds1 --mkfsoptions -I\ 128\\" >> $BATCHFILE
        # --mkfsoptions -I\ 128\
        do_lmc -m $XMLCONFIG --batch $BATCHFILE && return $?
        echo "backslash followed by nothing should return error"

        rm -f $BATCHFILE
        XMLCONFIG=$OLDXMLCONFIG
}
run_test 12 "lmc --batch, with single/double quote, backslash in batchfile"

test_13() {
        OLDXMLCONFIG=$XMLCONFIG
        XMLCONFIG="conf13-1.xml"

        # check long uuid will be truncated properly and uniquely
        echo "To generate XML configuration file(with long ost name): $XMLCONFIG"
        [ -f "$XMLCONFIG" ] && rm -f $XMLCONFIG
        do_lmc --add net --node $HOSTNAME --nid $HOSTNAME --nettype tcp
        do_lmc --add mds --node $HOSTNAME --mds mds1_name_longer_than_31characters
        do_lmc --add mds --node $HOSTNAME --mds mds2_name_longer_than_31characters
        if [ ! -f "$XMLCONFIG" ]; then
                echo "Error:no file $XMLCONFIG created!"
                return 1
        fi
        EXPECTEDMDS1UUID="e_longer_than_31characters_UUID"
        EXPECTEDMDS2UUID="longer_than_31characters_UUID_2"
        FOUNDMDS1UUID=`awk -F"'" '/<mds .*uuid=/' $XMLCONFIG | sed -n '1p' \
                       | sed "s/ /\n\r/g" | awk -F"'" '/uuid=/{print $2}'`
        FOUNDMDS2UUID=`awk -F"'" '/<mds .*uuid=/' $XMLCONFIG | sed -n '2p' \
                       | sed "s/ /\n\r/g" | awk -F"'" '/uuid=/{print $2}'`
	[ -z "$FOUNDMDS1UUID" ] && echo "MDS1 UUID empty" && return 1
	[ -z "$FOUNDMDS2UUID" ] && echo "MDS2 UUID empty" && return 1
        if ([ $EXPECTEDMDS1UUID = $FOUNDMDS1UUID ] && [ $EXPECTEDMDS2UUID = $FOUNDMDS2UUID ]) || \
           ([ $EXPECTEDMDS1UUID = $FOUNDMDS2UUID ] && [ $EXPECTEDMDS2UUID = $FOUNDMDS1UUID ]); then
                echo "Success:long uuid truncated successfully and being unique."
        else
                echo "Error:expected uuid for mds1 and mds2: $EXPECTEDMDS1UUID; $EXPECTEDMDS2UUID"
                echo "but:     found uuid for mds1 and mds2: $FOUNDMDS1UUID; $FOUNDMDS2UUID"
                return 1
        fi
        rm -f $XMLCONFIG
        XMLCONFIG=$OLDXMLCONFIG
}
run_test 13 "check new_uuid of lmc operating correctly"

test_13b() {
        OLDXMLCONFIG=$XMLCONFIG
        XMLCONFIG="conf13-1.xml"
        SECONDXMLCONFIG="conf13-2.xml"
        # check multiple invocations for lmc generate same XML configuration file
        rm -f $XMLCONFIG
        echo "Generate the first XML configuration file"
        gen_config
        echo "mv $XMLCONFIG to $SECONDXMLCONFIG"
        sed -e "s/mtime[^ ]*//" $XMLCONFIG > $SECONDXMLCONFIG || return $?
        echo "Generate the second XML configuration file"
        gen_config
	# don't compare .xml mtime, it will always be different
        if [ `sed -e "s/mtime[^ ]*//" $XMLCONFIG | diff - $SECONDXMLCONFIG | wc -l` -eq 0 ]; then
                echo "Success:multiple invocations for lmc generate same XML file"
        else
                echo "Error: multiple invocations for lmc generate different XML file"
                return 1
        fi

        rm -f $XMLCONFIG $SECONDXMLCONFIG
        XMLCONFIG=$OLDXMLCONFIG
}
run_test 13b "check lmc generates consistent .xml file"

test_14() {
        rm -f $XMLCONFIG

        # create xml file with --mkfsoptions for ost
        echo "create xml file with --mkfsoptions for ost"
        add_mds mds --dev $MDSDEV --size $MDSSIZE
        add_lov lov1 mds --stripe_sz $STRIPE_BYTES\
            --stripe_cnt $STRIPES_PER_OBJ --stripe_pattern 0
        add_ost ost --lov lov1 --dev $OSTDEV --size $OSTSIZE \
            --mkfsoptions "-Llabel_conf_14"
        add_client client mds --lov lov1 --path $MOUNT

        FOUNDSTRING=`awk -F"<" '/<mkfsoptions>/{print $2}' $XMLCONFIG`
        EXPECTEDSTRING="mkfsoptions>-Llabel_conf_14"
        if [ "$EXPECTEDSTRING" != "$FOUNDSTRING" ]; then
                echo "Error: expected: $EXPECTEDSTRING; found: $FOUNDSTRING"
                return 1
        fi
        echo "Success:mkfsoptions for ost written to xml file correctly."

        # mount lustre to test lconf mkfsoptions-parsing
        echo "mount lustre"
        start_ost
        start_mds
        mount_client $MOUNT || return $?
        if [ -z "`do_facet ost1 dumpe2fs -h $OSTDEV | grep label_conf_14`" ]; then
                echo "Error: the mkoptions not applied to mke2fs of ost."
                return 1
        fi
        cleanup
        echo "lconf mkfsoptions for ost success"

        gen_config
}
run_test 14 "test mkfsoptions of ost for lmc and lconf"

cleanup_15() {
	trap 0
	[ -f $MOUNTLUSTRE ] && echo "remove $MOUNTLUSTRE" && rm -f $MOUNTLUSTRE
	if [ -f $MOUNTLUSTRE.sav ]; then
		echo "return original $MOUNTLUSTRE.sav to $MOUNTLUSTRE"
		mv $MOUNTLUSTRE.sav $MOUNTLUSTRE
	fi
}

# this only tests the kernel mount command, not anything about lustre.
test_15() {
        MOUNTLUSTRE=${MOUNTLUSTRE:-/sbin/mount.lustre}
	start_ost
	start_mds

	echo "mount lustre on ${MOUNT} without $MOUNTLUSTRE....."
	if [ -f "$MOUNTLUSTRE" ]; then
		echo "save $MOUNTLUSTRE to $MOUNTLUSTRE.sav"
		mv $MOUNTLUSTRE $MOUNTLUSTRE.sav && trap cleanup_15 EXIT INT
		if [ -f $MOUNTLUSTRE ]; then
			skip "$MOUNTLUSTRE cannot be moved, skipping test"
			return 0
		fi
	fi

	mount_client $MOUNT && error "mount succeeded" && return 1
	echo "mount lustre on $MOUNT without $MOUNTLUSTRE failed as expected"
	cleanup_15
	cleanup || return $?
}
run_test 15 "zconf-mount without /sbin/mount.lustre (should return error)"

test_16() {
        TMPMTPT="${MOUNT%/*}/conf16"

        if [ ! -e "$MDSDEV" ]; then
            log "no $MDSDEV existing, so mount Lustre to create one"
            setup
            check_mount || return 41
            cleanup || return $?
        fi

        [ -f "$MDSDEV" ] && LOOPOPT="-o loop"

        log "change the mode of $MDSDEV/OBJECTS,LOGS,PENDING to 555"
        do_facet mds "mkdir -p $TMPMTPT &&
                      mount $LOOPOPT -t $FSTYPE $MDSDEV $TMPMTPT &&
                      chmod 555 $TMPMTPT/{OBJECTS,LOGS,PENDING} &&
                      umount $TMPMTPT" || return $?

        log "mount Lustre to change the mode of OBJECTS/LOGS/PENDING, then umount Lustre"
	setup
        check_mount || return 41
        cleanup || return $?

        log "read the mode of OBJECTS/LOGS/PENDING and check if they has been changed properly"
        EXPECTEDOBJECTSMODE=`do_facet mds "debugfs -R 'stat OBJECTS' $MDSDEV 2> /dev/null" | grep 'Mode: ' | sed -e "s/.*Mode: *//" -e "s/ *Flags:.*//"`
        EXPECTEDLOGSMODE=`do_facet mds "debugfs -R 'stat LOGS' $MDSDEV 2> /dev/null" | grep 'Mode: ' | sed -e "s/.*Mode: *//" -e "s/ *Flags:.*//"`
        EXPECTEDPENDINGMODE=`do_facet mds "debugfs -R 'stat PENDING' $MDSDEV 2> /dev/null" | grep 'Mode: ' | sed -e "s/.*Mode: *//" -e "s/ *Flags:.*//"`

        if [ "$EXPECTEDOBJECTSMODE" = "0777" ]; then
                log "Success:Lustre change the mode of OBJECTS correctly"
        else
                error "Lustre does not change mode of OBJECTS properly"
        fi

        if [ "$EXPECTEDLOGSMODE" = "0777" ]; then
                log "Success:Lustre change the mode of LOGS correctly"
        else
                error "Lustre does not change mode of LOGS properly"
        fi

        if [ "$EXPECTEDPENDINGMODE" = "0777" ]; then
                log "Success:Lustre change the mode of PENDING correctly"
        else
                error "Lustre does not change mode of PENDING properly"
        fi
}
run_test 16 "verify that lustre will correct the mode of OBJECTS/LOGS/PENDING"

test_17() {
        if [ ! -e "$MDSDEV" ]; then
            echo "no $MDSDEV existing, so mount Lustre to create one"
	    setup
            check_mount || return 41
            cleanup || return $?
        fi

        echo "Remove mds config log"
        do_facet mds "debugfs -w -R 'unlink CONFIGS/$FSNAME-MDT0000' $MDSDEV || return \$?" || return $?

        start_ost
	start_mds && return 42
	gen_config
}
run_test 17 "Verify failed mds_postsetup won't fail assertion (2936) (should return errs)"

test_18() {
        [ -f $MDSDEV ] && echo "remove $MDSDEV" && rm -f $MDSDEV
        echo "mount mds with large journal..."
        local myMDSSIZE=2000000
        OLD_MDS_MKFS_OPTS=$MDS_MKFS_OPTS

        MDS_MKFS_OPTS="--mgs --mdt --fsname=$FSNAME --device-size=$myMDSSIZE --param sys.timeout=$TIMEOUT $MDSOPT"

        gen_config
        echo "mount lustre system..."
	setup
        check_mount || return 41

        echo "check journal size..."
        FOUNDSIZE=`do_facet mds "debugfs -c -R 'stat <8>' $MDSDEV" | awk '/Size: / { print $NF; exit;}'`
        if [ $FOUNDSIZE -gt $((32 * 1024 * 1024)) ]; then
                log "Success: mkfs creates large journals. Size: $((FOUNDSIZE >> 20))M"
        else
                error "expected journal size > 32M, found $((FOUNDSIZE >> 20))M"
        fi

        cleanup || return $?

        MDS_MKFS_OPTS=$OLD_MDS_MKFS_OPTS
        gen_config
}
run_test 18 "check mkfs creates large journals"

test_19a() {
	start_mds || return 1
	stop_mds -f || return 2
}
run_test 19a "start/stop MDS without OSTs"

test_19b() {
	start_ost || return 1
	stop_ost -f || return 2
}
run_test 19b "start/stop OSTs without MDS"

test_20() {
	# first format the ost/mdt
	start_ost
	start_mds
	mount_client $MOUNT
	check_mount || return 43
	rm -f $DIR/$tfile
	remount_client ro $MOUNT || return 44
	touch $DIR/$tfile && echo "$DIR/$tfile created incorrectly" && return 45
	[ -e $DIR/$tfile ] && echo "$DIR/$tfile exists incorrectly" && return 46
	remount_client rw $MOUNT || return 47
	touch $DIR/$tfile
	[ ! -f $DIR/$tfile ] && echo "$DIR/$tfile missing" && return 48
	MCNT=`grep -c $MOUNT /etc/mtab`
	[ "$MCNT" -ne 1 ] && echo "$MOUNT in /etc/mtab $MCNT times" && return 49
	umount_client $MOUNT
	stop_mds
	stop_ost
}
run_test 20 "remount ro,rw mounts work and doesn't break /etc/mtab"

test_21a() {
        start_mds
	start_ost
	stop_ost
	stop_mds
}
run_test 21a "start mds before ost, stop ost first"

test_21b() {
        start_ost
	start_mds
	stop_mds
	stop_ost
}
run_test 21b "start ost before mds, stop mds first"

test_21c() {
        start_ost
	start_mds
	start_ost2
	stop_ost
	stop_ost2
	stop_mds
}
run_test 21c "start mds between two osts, stop mds last"

test_22() {
        #reformat to remove all logs
        reformat
	start_mds
	echo Client mount before any osts are in the logs
	mount_client $MOUNT
	check_mount && return 41
	pass

	echo Client mount with ost in logs, but none running
	start_ost
	stop_ost
	mount_client $MOUNT
	# check_mount will block trying to contact ost
	umount_client $MOUNT
	pass

	echo Client mount with a running ost
	start_ost
	mount_client $MOUNT
	check_mount || return 41
	pass

	cleanup
}
run_test 22 "start a client before osts (should return errs)"

test_23() {
        setup
        # fail mds
	stop mds   
	# force down client so that recovering mds waits for reconnect
	zconf_umount `hostname` $MOUNT -f
	# enter recovery on mds
	start_mds
	# try to start a new client
	mount_client $MOUNT &
	MOUNT_PID=$!
	sleep 5
	MOUNT_LUSTRE_PID=`ps -ef | grep mount.lustre | grep -v grep | awk '{print $2}'`
	echo mount pid is ${MOUNT_PID}, mount.lustre pid is ${MOUNT_LUSTRE_PID}
	ps --ppid $MOUNT_PID
	ps --ppid $MOUNT_LUSTRE_PID
	# FIXME why o why can't I kill these? Manual "ctrl-c" works...
	kill -TERM $MOUNT_PID
	echo "waiting for mount to finish"
	ps -ef | grep mount
	wait $MOUNT_PID

	stop_mds
	stop_ost
}
#this test isn't working yet
#run_test 23 "interrupt client during recovery mount delay"

test_24a() {
	local fs2mds_HOST=$mds_HOST
	local fs2ost_HOST=$ost_HOST

	[ -z "$fs2ost_DEV" -o -z "$fs2mds_DEV" ] && [ -b "$MDSDEV" ] && \
            log "mixed loopback and real device not working" && return

	local fs2mdsdev=${fs2mds_DEV:-${MDSDEV}_2}
	local fs2ostdev=${fs2ost_DEV:-$(ostdevname 1)_2}

	# test 8-char fsname as well
	local FSNAME2=test1234
	add fs2mds $MDS_MKFS_OPTS --fsname=${FSNAME2} --nomgs --mgsnode=$MGSNID --reformat $fs2mdsdev || exit 10

	add fs2ost $OST_MKFS_OPTS --fsname=${FSNAME2} --reformat $fs2ostdev || exit 10

	setup
	start fs2mds $fs2mdsdev $MDS_MOUNT_OPTS
	start fs2ost $fs2ostdev $OST_MOUNT_OPTS
	mkdir -p $MOUNT2
	mount -t lustre $MGSNID:/${FSNAME2} $MOUNT2 || return 1
	# 1 still works
	check_mount || return 2
	# files written on 1 should not show up on 2
	cp /etc/passwd $DIR/$tfile
	sleep 10
	[ -e $MOUNT2/$tfile ] && error "File bleed" && return 7
	# 2 should work
	cp /etc/passwd $MOUNT2/b || return 3
	rm $MOUNT2/b || return 4
	# 2 is actually mounted
        grep $MOUNT2' ' /proc/mounts > /dev/null || return 5
	# failover 
	facet_failover fs2mds
	facet_failover fs2ost
	df
 	umount_client $MOUNT 
	# the MDS must remain up until last MDT
	stop_mds
	MDS=$(do_facet mds "cat $LPROC/devices" | awk '($3 ~ "mdt" && $4 ~ "MDS") { print $4 }')
	[ -z "$MDS" ] && error "No MDS" && return 8
	umount $MOUNT2
	stop fs2mds -f
	stop fs2ost -f
	cleanup_nocli || return 6
}
run_test 24a "Multiple MDTs on a single node"

test_24b() {
	local fs2mds_HOST=$mds_HOST
        [ -z "$fs2mds_DEV" ] && [ -b "$MDSDEV" ] && \
            log "mixed loopback and real device not working" && return

	local fs2mdsdev=${fs2mds_DEV:-${MDSDEV}_2}

        add fs2mds $MDS_MKFS_OPTS --fsname=${FSNAME}2 --mgs --reformat $fs2mdsdev || exit 10 
	setup
	start fs2mds $fs2mdsdev $MDS_MOUNT_OPTS && return 2
	cleanup || return 6
}
run_test 24b "Multiple MGSs on a single node (should return err)"

test_25() {
	setup
	check_mount || return 2
	local MODULES=$($LCTL modules | awk '{ print $2 }')
	rmmod $MODULES 2>/dev/null || true
	cleanup || return 6
}
run_test 25 "Verify modules are referenced"

test_26() {
    load_modules
    # we need modules before mount for sysctl, so make sure...
    do_facet mds "lsmod | grep -q lustre || modprobe lustre"
#define OBD_FAIL_MDS_FS_SETUP            0x135
    do_facet mds "sysctl -w lustre.fail_loc=0x80000135"
    start_mds && echo MDS started && return 1
    cat $LPROC/devices
    DEVS=$(cat $LPROC/devices | wc -l)
    [ $DEVS -gt 0 ] && return 2
    unload_modules || return 203
}
run_test 26 "MDT startup failure cleans LOV (should return errs)"

set_and_check() {
	local myfacet=$1
	local TEST=$2
	local PARAM=$3
	local ORIG=$(do_facet $myfacet "$TEST") 
	if [ $# -gt 3 ]; then
	    local FINAL=$4
	else
	    local -i FINAL
	    FINAL=$(($ORIG + 5))
	fi
	echo "Setting $PARAM from $ORIG to $FINAL"
	do_facet mds "$LCTL conf_param $PARAM=$FINAL" || error conf_param failed
	local RESULT
	local MAX=90
	local WAIT=0
	while [ 1 ]; do
	    sleep 5
	    RESULT=$(do_facet $myfacet "$TEST") 
	    if [ $RESULT -eq $FINAL ]; then
		echo "Updated config after $WAIT sec (got $RESULT)"
		break
	    fi
	    WAIT=$((WAIT + 5))
	    if [ $WAIT -eq $MAX ]; then
		echo "Config update not seen: wanted $FINAL got $RESULT"
		return 3
	    fi
	    echo "Waiting $(($MAX - $WAIT)) secs for config update" 
	done
}

test_27a() {
	start_ost || return 1
	start_mds || return 2
	echo "Requeue thread should have started: " 
	ps -e | grep ll_cfg_requeue 
	set_and_check ost1 "cat $LPROC/obdfilter/$FSNAME-OST0000/client_cache_seconds" "$FSNAME-OST0000.ost.client_cache_seconds" || return 3 
	cleanup_nocli
}
run_test 27a "Reacquire MGS lock if OST started first"

test_27b() {
        setup
	facet_failover mds
	set_and_check mds "cat $LPROC/mds/$FSNAME-MDT0000/group_acquire_expire" "$FSNAME-MDT0000.mdt.group_acquire_expire" || return 3 
	set_and_check client "cat $LPROC/mdc/$FSNAME-MDT0000-mdc-*/max_rpcs_in_flight" "$FSNAME-MDT0000.mdc.max_rpcs_in_flight" || return 4 
	cleanup
}
run_test 27b "Reacquire MGS lock after failover"

test_28() {
        setup
	TEST="cat $LPROC/llite/$FSNAME-*/max_read_ahead_whole_mb"
	ORIG=$($TEST) 
	declare -i FINAL
	FINAL=$(($ORIG + 10))
	set_and_check client "$TEST" "$FSNAME.llite.max_read_ahead_whole_mb" || return 3
	set_and_check client "$TEST" "$FSNAME.llite.max_read_ahead_whole_mb" || return 3
 	umount_client $MOUNT || return 200
	mount_client $MOUNT
	RESULT=$($TEST)
	if [ $RESULT -ne $FINAL ]; then
	    echo "New config not seen: wanted $FINAL got $RESULT"
	    return 4
	else
	    echo "New config success: got $RESULT"
	fi
	cleanup
}
run_test 28 "permanent parameter setting"

test_29() {
	[ "$OSTCOUNT" -lt "2" ] && skip "$OSTCOUNT < 2, skipping" && return
        setup > /dev/null 2>&1
	start_ost2
	sleep 10

	local PARAM="$FSNAME-OST0001.osc.active"
	local PROC_ACT="$LPROC/osc/$FSNAME-OST0001-osc-*/active"
	local PROC_UUID="$LPROC/osc/$FSNAME-OST0001-osc-*/ost_server_uuid"
	if [ ! -r $PROC_ACT ]; then
	    echo "Can't read $PROC_ACT"
	    ls $LPROC/osc/$FSNAME-*
	    return 1
	fi
	ACTV=$(cat $PROC_ACT)
	DEAC=$((1 - $ACTV))
	set_and_check client "cat $PROC_ACT" "$PARAM" $DEAC || return 2
        # also check ost_server_uuid status
	RESULT=$(grep DEACTIV $PROC_UUID)
	if [ -z "$RESULT" ]; then
	    echo "Live client not deactivated: $(cat $PROC_UUID)"
	    return 3
	else
	    echo "Live client success: got $RESULT"
	fi

	# check MDT too 
	local MPROC="$LPROC/osc/$FSNAME-OST0001-osc/active"
	local MAX=30
	local WAIT=0
	while [ 1 ]; do
	    sleep 5
	    RESULT=`do_facet mds " [ -r $MPROC ] && cat $MPROC"`
	    [ ${PIPESTATUS[0]} = 0 ] || error "Can't read $MPROC"
	    if [ $RESULT -eq $DEAC ]; then
		echo "MDT deactivated also after $WAIT sec (got $RESULT)"
		break
	    fi
	    WAIT=$((WAIT + 5))
	    if [ $WAIT -eq $MAX ]; then
		echo "MDT not deactivated: wanted $DEAC got $RESULT"
		return 4
	    fi
	    echo "Waiting $(($MAX - $WAIT)) secs for MDT deactivated"
	done

        # test new client starts deactivated
 	umount_client $MOUNT || return 200
	mount_client $MOUNT
	RESULT=$(grep DEACTIV $PROC_UUID | grep NEW)
	if [ -z "$RESULT" ]; then
	    echo "New client not deactivated from start: $(cat $PROC_UUID)"
	    return 5
	else
	    echo "New client success: got $RESULT"
	fi

	# make sure it reactivates
	set_and_check client "cat $PROC_ACT" "$PARAM" $ACTV || return 6

 	umount_client $MOUNT
	stop_ost2
	cleanup_nocli
	#writeconf to remove all ost2 traces for subsequent tests
	writeconf
}
run_test 29 "permanently remove an OST"

test_30() {
	# start mds first after writeconf
	start_mds
	start_ost
	mount_client $MOUNT
	TEST="cat $LPROC/llite/$FSNAME-*/max_read_ahead_whole_mb"
	ORIG=$($TEST) 
	for i in $(seq 1 20); do 
	    set_and_check client "$TEST" "$FSNAME.llite.max_read_ahead_whole_mb" $i || return 3
	done
	# make sure client restart still works 
 	umount_client $MOUNT
	mount_client $MOUNT || return 4
	[ "$($TEST)" -ne "$i" ] && return 5   
	set_and_check client "$TEST" "$FSNAME.llite.max_read_ahead_whole_mb" $ORIG || return 6
	cleanup
}
run_test 30 "Big config llog"

test_31() { # bug 10734
        # ipaddr must not exist
        mount -t lustre 4.3.2.1@tcp:/lustre $MOUNT || true
	cleanup
}
run_test 31 "Connect to non-existent node (returns errors, should not crash)"

test_32a() {
        # XXX - make this run on client-only systems with real hardware on
        #       the OST and MDT
        #       there appears to be a lot of assumption here about loopback
        #       devices
        # or maybe this test is just totally useless on a client-only system
        [ -z "$TUNEFS" ] && skip "No tunefs" && return
	local DISK1_4=$LUSTRE/tests/disk1_4.zip
        [ ! -r $DISK1_4 ] && skip "Cant find $DISK1_4, skipping" && return
	unzip -o -j -d $TMP/$tdir $DISK1_4 || { skip "Cant unzip $DISK1_4, skipping" && return ; }
	load_modules
	sysctl lnet.debug=$PTLDEBUG

	$TUNEFS $TMP/$tdir/mds || error "tunefs failed"
	# nids are wrong, so client wont work, but server should start
        start mds $TMP/$tdir/mds "-o loop,exclude=lustre-OST0000" || return 3
        local UUID=$(cat $LPROC/mds/lustre-MDT0000/uuid)
	echo MDS uuid $UUID
	[ "$UUID" == "mdsA_UUID" ] || error "UUID is wrong: $UUID" 

	$TUNEFS --mgsnode=`hostname` $TMP/$tdir/ost1 || error "tunefs failed"
	start ost1 $TMP/$tdir/ost1 "-o loop" || return 5
        UUID=$(cat $LPROC/obdfilter/lustre-OST0000/uuid)
	echo OST uuid $UUID
	[ "$UUID" == "ost1_UUID" ] || error "UUID is wrong: $UUID" 

	local NID=$($LCTL list_nids | head -1)

	echo "OSC changes should return err:" 
	$LCTL conf_param lustre-OST0000.osc.max_dirty_mb=15 && return 7
	$LCTL conf_param lustre-OST0000.failover.node=$NID && return 8
	echo "ok."
	echo "MDC changes should succeed:" 
	$LCTL conf_param lustre-MDT0000.mdc.max_rpcs_in_flight=9 || return 9
	$LCTL conf_param lustre-MDT0000.failover.node=$NID || return 10
	echo "ok."

	# With a new good MDT failover nid, we should be able to mount a client
	# (but it cant talk to OST)
        local OLDMOUNTOPT=$MOUNTOPT
        MOUNTOPT="exclude=lustre-OST0000"
	mount_client $MOUNT
        MOUNTOPT=$OLDMOUNTOPT
	set_and_check client "cat $LPROC/mdc/*/max_rpcs_in_flight" "lustre-MDT0000.mdc.max_rpcs_in_flight" || return 11

	zconf_umount `hostname` $MOUNT -f
	cleanup_nocli
	load_modules

        # mount a second time to make sure we didnt leave upgrade flag on
	load_modules
        $TUNEFS --dryrun $TMP/$tdir/mds || error "tunefs failed"
	load_modules
        start mds $TMP/$tdir/mds "-o loop,exclude=lustre-OST0000" || return 12
        cleanup_nocli

	[ -d $TMP/$tdir ] && rm -rf $TMP/$tdir
}
run_test 32a "Upgrade from 1.4 (not live)"

test_32b() {
        # XXX - make this run on client-only systems with real hardware on
        #       the OST and MDT
        #       there appears to be a lot of assumption here about loopback
        #       devices
        # or maybe this test is just totally useless on a client-only system
        [ -z "$TUNEFS" ] && skip "No tunefs" && return
	local DISK1_4=$LUSTRE/tests/disk1_4.zip
        [ ! -r $DISK1_4 ] && skip "Cant find $DISK1_4, skipping" && return
	unzip -o -j -d $TMP/$tdir $DISK1_4 || { skip "Cant unzip $DISK1_4, skipping" && return ; }
	load_modules
	sysctl lnet.debug=$PTLDEBUG
	NEWNAME=sofia

	# writeconf will cause servers to register with their current nids
	$TUNEFS --writeconf --fsname=$NEWNAME $TMP/$tdir/mds || error "tunefs failed"
	start mds $TMP/$tdir/mds "-o loop" || return 3
        local UUID=$(cat $LPROC/mds/${NEWNAME}-MDT0000/uuid)
	echo MDS uuid $UUID
	[ "$UUID" == "mdsA_UUID" ] || error "UUID is wrong: $UUID" 

	$TUNEFS --mgsnode=`hostname` --fsname=$NEWNAME --writeconf $TMP/$tdir/ost1 || error "tunefs failed"
	start ost1 $TMP/$tdir/ost1 "-o loop" || return 5
        UUID=$(cat $LPROC/obdfilter/${NEWNAME}-OST0000/uuid)
	echo OST uuid $UUID
	[ "$UUID" == "ost1_UUID" ] || error "UUID is wrong: $UUID"

	echo "OSC changes should succeed:" 
	$LCTL conf_param ${NEWNAME}-OST0000.osc.max_dirty_mb=15 || return 7
	$LCTL conf_param ${NEWNAME}-OST0000.failover.node=$NID || return 8
	echo "ok."
	echo "MDC changes should succeed:" 
	$LCTL conf_param ${NEWNAME}-MDT0000.mdc.max_rpcs_in_flight=9 || return 9
	echo "ok."

	# MDT and OST should have registered with new nids, so we should have
	# a fully-functioning client
	echo "Check client and old fs contents"
	OLDFS=$FSNAME
	FSNAME=$NEWNAME
	mount_client $MOUNT
	FSNAME=$OLDFS
	set_and_check client "cat $LPROC/mdc/*/max_rpcs_in_flight" "${NEWNAME}-MDT0000.mdc.max_rpcs_in_flight" || return 11
	[ "$(cksum $MOUNT/passwd | cut -d' ' -f 1,2)" == "2479747619 779" ] || return 12  
	echo "ok."

	cleanup
	[ -d $TMP/$tdir ] && rm -rf $TMP/$tdir
}
run_test 32b "Upgrade from 1.4 with writeconf"

test_33() { # bug 12333
        local FSNAME2=test1234
        local fs2mds_HOST=$mds_HOST
        local fs2ost_HOST=$ost_HOST

        [ -z "$fs2ost_DEV" -o -z "$fs2mds_DEV" ] && [ -b "$MDSDEV" ] && \
            log "mixed loopback and real device not working" && return

        local fs2mdsdev=${fs2mds_DEV:-${MDSDEV}_2}
        local fs2ostdev=${fs2ost_DEV:-$(ostdevname 1)_2}
        add fs2mds $MDS_MKFS_OPTS --fsname=${FSNAME2} --reformat $fs2mdsdev || exit 10
        add fs2ost $OST_MKFS_OPTS --fsname=${FSNAME2} --index=8191 --mgsnode=$MGSNID --reformat $fs2ostdev || exit 10

        start fs2mds $fs2mdsdev $MDS_MOUNT_OPTS
        start fs2ost $fs2ostdev $OST_MOUNT_OPTS
        mkdir -p $MOUNT2
        mount -t lustre $MGSNID:/${FSNAME2} $MOUNT2 || return 1
        echo "ok."

        umount -d $MOUNT2
        stop fs2ost -f
        stop fs2mds -f
        rm -rf $MOUNT2 $fs2mdsdev $fs2ostdev
        cleanup_nocli || return 6
}
run_test 33 "Mount ost with a large index number"

umount_client $MOUNT	
cleanup_nocli

test_33a() {
        setup

        do_facet client dd if=/dev/zero of=$MOUNT/24 bs=1024k count=1
        # Drop lock cancelation reply during umount
	#define OBD_FAIL_LDLM_CANCEL             0x304
        do_facet client sysctl -w lustre.fail_loc=0x80000304
        #sysctl -w lnet.debug=-1
        umount_client $MOUNT
        cleanup
}
run_test 33a "Drop cancel during umount"

test_34a() {
        setup
	do_facet client multiop $DIR/file O_c &
	sleep 0.500s
	manual_umount_client
	rc=$?
	do_facet client killall -USR1 multiop
	if [ $rc -eq 0 ]; then
		error "umount not fail!"
	fi
	sleep 1
        cleanup
}
run_test 34a "umount with opened file should be fail"


test_34b() {
	setup
	touch $DIR/$tfile || return 1
	stop_mds --force || return 2

 	manual_umount_client --force
	rc=$?
	if [ $rc -ne 0 ]; then
		error "mtab after failed umount - rc $rc"
	fi

	cleanup
	return 0	
}
run_test 34b "force umount with failed mds should be normal"

test_34c() {
	setup
	touch $DIR/$tfile || return 1
	stop_ost --force || return 2

 	manual_umount_client --force
	rc=$?
	if [ $rc -ne 0 ]; then
		error "mtab after failed umount - rc $rc"
	fi

	cleanup
	return 0	
}
run_test 34c "force umount with failed mds should be normal"

test_35() { # bug 12459
	setup

	debugsave
	sysctl -w lnet.debug="ha"

	log "Set up a fake failnode for the MDS"
	FAKENID="127.0.0.2"
	do_facet mds $LCTL conf_param ${FSNAME}-MDT0000.failover.node=$FAKENID || return 4

	log "Wait for RECONNECT_INTERVAL seconds (10s)"
	sleep 10

	MSG="conf-sanity.sh test_33 `date +%F%kh%Mm%Ss`"
	$LCTL clear
	log "$MSG"
	log "Stopping the MDT:"
	stop_mds || return 5

	df $MOUNT > /dev/null 2>&1 &
	DFPID=$!
	log "Restarting the MDT:"
	start_mds || return 6
	log "Wait for df ($DFPID) ... "
	wait $DFPID
	log "done"
	debugrestore

	# retrieve from the log the first server that the client tried to
	# contact after the connection loss
	$LCTL dk $TMP/lustre-log-$TESTNAME.log
	NEXTCONN=`awk "/${MSG}/ {start = 1;}
		       /import_select_connection.*${FSNAME}-MDT0000-mdc.* using connection/ {
				if (start) {
					if (\\\$NF ~ /$FAKENID/)
						print \\\$NF;
					else
						print 0;
					exit;
				}
		       }" $TMP/lustre-log-$TESTNAME.log`
	[ "$NEXTCONN" != "0" ] && log "The client didn't try to reconnect to the last active server (tried ${NEXTCONN} instead)" && return 7
	cleanup
}
run_test 35 "Reconnect to the last active server first"

test_36() { # 12743
        local rc
        local FSNAME2=test1234
        local fs2mds_HOST=$mds_HOST
        local fs2ost_HOST=$ost_HOST
        local fs3ost_HOST=$ost_HOST
        rc=0

        [ -z "$fs2ost_DEV" -o -z "$fs2mds_DEV" ] && [ -b "$MDSDEV" ] && \
            log "mixed loopback and real device not working" && return

        [ $OSTCOUNT -lt 2 ] && skip "skipping test for single OST" && return

        local fs2mdsdev=${fs2mds_DEV:-${MDSDEV}_2}
        local fs2ostdev=${fs2ost_DEV:-$(ostdevname 1)_2}
        local fs3ostdev=${fs3ost_DEV:-$(ostdevname 2)_2}
        add fs2mds $MDS_MKFS_OPTS --fsname=${FSNAME2} --reformat $fs2mdsdev || exit 10
        add fs2ost $OST_MKFS_OPTS --mkfsoptions='-b1024' --fsname=${FSNAME2} --mgsnode=`hostname`@tcp --reformat $fs2ostdev || exit 10
        add fs3ost $OST_MKFS_OPTS --mkfsoptions='-b4096' --fsname=${FSNAME2} --mgsnode=`hostname`@tcp --reformat $fs3ostdev || exit 10

        start fs2mds $fs2mdsdev $MDS_MOUNT_OPTS
        start fs2ost $fs2ostdev $OST_MOUNT_OPTS
        start fs3ost $fs3ostdev $OST_MOUNT_OPTS
        mkdir -p $MOUNT2
        mount -t lustre $MGSNID:/${FSNAME2} $MOUNT2 || return 1

        dd if=/dev/zero of=$MOUNT2/$tfile bs=1M count=7 || return 2
        [ $(grep -c obdfilter $LPROC/devices) -eq 0 ] &&
                skip "skipping test for remote OST" && return
        BKTOTAL=`awk 'BEGIN{total=0}; {total+=$1}; END{print total}' \
                $LPROC/obdfilter/*/kbytestotal`
        BKFREE=`awk 'BEGIN{free=0}; {free+=$1}; END{print free}' \
               $LPROC/obdfilter/*/kbytesfree`
        BKAVAIL=`awk 'BEGIN{avail=0}; {avail+=$1}; END{print avail}' \
                $LPROC/obdfilter/*/kbytesavail`
        STRING=`df -P $MOUNT2 | tail -n 1 | awk '{print $2","$3","$4}'`
        DFTOTAL=`echo $STRING | cut -d, -f1`
        DFUSED=`echo $STRING  | cut -d, -f2`
        DFAVAIL=`echo $STRING | cut -d, -f3`
        DFFREE=$(($DFTOTAL - $DFUSED))

        ALLOWANCE=$((64 * $OSTCOUNT))

        if [ $DFTOTAL -lt $(($BKTOTAL - $ALLOWANCE)) ] ||  
           [ $DFTOTAL -gt $(($BKTOTAL + $ALLOWANCE)) ] ; then
                echo "**** FAIL: df total($DFTOTAL) mismatch OST total($BKTOTAL)"
                rc=1
        fi
        if [ $DFFREE -lt $(($BKFREE - $ALLOWANCE)) ] || 
           [ $DFFREE -gt $(($BKFREE + $ALLOWANCE)) ] ; then
                echo "**** FAIL: df free($DFFREE) mismatch OST free($BKFREE)"
                rc=2
        fi
        if [ $DFAVAIL -lt $(($BKAVAIL - $ALLOWANCE)) ] || 
           [ $DFAVAIL -gt $(($BKAVAIL + $ALLOWANCE)) ] ; then
                echo "**** FAIL: df avail($DFAVAIL) mismatch OST avail($BKAVAIL)"
                rc=3
       fi

        umount -d $MOUNT2
        stop fs3ost -f || return 200
        stop fs2ost -f || return 201
        stop fs2mds -f || return 202
        rm -rf $MOUNT2 $fs2mdsdev $fs2ostdev $fs3ostdev
        unload_modules || return 203
        return $rc
}
run_test 36 "df report consistency on OSTs with different block size"

equals_msg `basename $0`: test complete
[ -f "$TESTSUITELOG" ] && cat $TESTSUITELOG || true
