#!/bin/bash

set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped test: 1768 3192
ALWAYS_EXCEPT=${ALWAYS_EXCEPT:-"4   14b 14c"}
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

[ "$ALWAYS_EXCEPT$EXCEPT" ] && echo "Skipping tests: $ALWAYS_EXCEPT $EXCEPT"

SRCDIR=`dirname $0`
PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH

CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFIND=${LFIND:-lfind}
LSTRIPE=${LSTRIPE:-lstripe}
LCTL=${LCTL:-lctl}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
TOEXCL=${TOEXCL:-toexcl}
TRUNCATE=${TRUNCATE:-truncate}

if [ $UID -ne 0 ]; then
	RUNAS_ID="$UID"
	RUNAS=""
else
	RUNAS_ID=${RUNAS_ID:-500}
	RUNAS=${RUNAS:-"runas -u $RUNAS_ID"}
fi

SAVE_PWD=$PWD

clean() {
	echo -n "cln.."
	sh llmountcleanup.sh > /dev/null || exit 20
}
CLEAN=${CLEAN:-}

start() {
	echo -n "mnt.."
	sh llrmount.sh > /dev/null || exit 10
	echo "done"
}
START=${START:-}

log() {
	echo "$*"
	lctl mark "$*" 2> /dev/null || true
}

run_one() {
	if ! mount | grep -q $DIR1; then
		$START
	fi
	log "== test $1: $2"
	export TESTNAME=test_$1
	test_$1 || error "test_$1: exit with rc=$?"
	unset TESTNAME
	pass
	cd $SAVE_PWD
	$CLEAN
}

run_test() {
	for O in $ONLY; do
		if [ "`echo $1 | grep '\<'$O'[a-z]*\>'`" ]; then
			echo ""
			run_one $1 "$2"
			return $?
		else
			echo -n "."
		fi
	done
	for X in $EXCEPT $ALWAYS_EXCEPT; do
		if [ "`echo $1 | grep '\<'$X'[a-z]*\>'`" ]; then
			echo "skipping excluded test $1"
			return 0
		fi
	done
	if [ -z "$ONLY" ]; then
		run_one $1 "$2"
		return $?
	fi
}

[ "$SANITYLOG" ] && rm -f $SANITYLOG || true

error () {
	log "FAIL: $TESTNAME $@"
	if [ "$SANITYLOG" ]; then
		echo "FAIL: $TESTNAME $@" >> $SANITYLOG
	else
		exit 1
	fi
}

pass() {
	echo PASS
}

export MOUNT1=`mount| awk '/ lustre/ { print $3 }'| head -n 1`
export MOUNT2=`mount| awk '/ lustre/ { print $3 }'| tail -n 1`
[ -z "$MOUNT1" ] && error "NAME=$NAME not mounted once"
[ "$MOUNT1" = "$MOUNT2" ] && error "NAME=$NAME not mounted twice"
[ `mount| awk '/ lustre/ { print $3 }'| wc -l` -ne 2 ] && \
	error "NAME=$NAME mounted more than twice"

export DIR1=${DIR1:-$MOUNT1}
export DIR2=${DIR2:-$MOUNT2}
[ -z "`echo $DIR1 | grep $MOUNT1`" ] && echo "$DIR1 not in $MOUNT1" && exit 96
[ -z "`echo $DIR2 | grep $MOUNT2`" ] && echo "$DIR2 not in $MOUNT2" && exit 95

rm -rf $DIR1/[df][0-9]* $DIR1/lnk

test_1a() {
	touch $DIR1/f1
	[ -f $DIR2/f1 ] || error
}
run_test 1a "check create on 2 mtpt's =========================="

test_1b() {
	chmod 777 $DIR2/f1
	$CHECKSTAT -t file -p 0777 $DIR1/f1 || error
	chmod a-x $DIR2/f1
}
run_test 1b "check attribute updates on 2 mtpt's ==============="

test_1c() {
	$CHECKSTAT -t file -p 0666 $DIR1/f1 || error
}
run_test 1c "check after remount attribute updates on 2 mtpt's ="

test_1d() {
	rm $DIR2/f1
	$CHECKSTAT -a $DIR1/f1 || error
}
run_test 1d "unlink on one mountpoint removes file on other ===="

test_2a() {
	touch $DIR1/f2a
	ls -l $DIR2/f2a
	chmod 777 $DIR2/f2a
	$CHECKSTAT -t file -p 0777 $DIR1/f2a || error
}
run_test 2a "check cached attribute updates on 2 mtpt's ========"

test_2b() {
	touch $DIR1/f2b
	ls -l $DIR2/f2b
	chmod 777 $DIR1/f2b
	$CHECKSTAT -t file -p 0777 $DIR2/f2b || error
}
run_test 2b "check cached attribute updates on 2 mtpt's ========"

# NEED TO SAVE ROOT DIR MODE
test_2c() {
	chmod 777 $DIR1
	$CHECKSTAT -t dir -p 0777 $DIR2 || error
}
run_test 2c "check cached attribute updates on 2 mtpt's root ==="

test_2d() {
	chmod 755 $DIR1
	$CHECKSTAT -t dir -p 0755 $DIR2 || error
}
run_test 2d "check cached attribute updates on 2 mtpt's root ==="

test_3() {
	( cd $DIR1 ; ln -s this/is/good lnk )
	[ "this/is/good" = "`perl -e 'print readlink("'$DIR2/lnk'");'`" ] || \
		error
}
run_test 3 "symlink on one mtpt, readlink on another ==========="

test_4() {
	multifstat $DIR1/f4 $DIR2/f4
}
run_test 4 "fstat validation on multiple mount points =========="

test_5() {
	mcreate $DIR1/f5
	truncate $DIR2/f5 100
	$CHECKSTAT -t file -s 100 $DIR1/f5 || error
	rm $DIR1/f5
}
run_test 5 "create a file on one mount, truncate it on the other"

test_6() {
	openunlink $DIR1/f6 $DIR2/f6 || error
}
run_test 6 "remove of open file on other node =================="

test_7() {
	opendirunlink $DIR1/d7 $DIR2/d7 || error
}
run_test 7 "remove of open directory on other node ============="

test_8() {
	opendevunlink $DIR1/dev8 $DIR2/dev8 || error
}
run_test 8 "remove of open special file on other node =========="

test_9() {
	MTPT=1
	> $DIR2/f9
	for C in a b c d e f g h i j k l; do
		DIR=`eval echo \\$DIR$MTPT`
		echo -n $C >> $DIR/f9
		[ "$MTPT" -eq 1 ] && MTPT=2 || MTPT=1
	done
	[ "`cat $DIR1/f9`" = "abcdefghijkl" ] || \
		error "`od -a $DIR1/f9` != abcdefghijkl"
}
run_test 9 "append of file with sub-page size on multiple mounts"

test_10a() {
	MTPT=1
	OFFSET=0
	> $DIR2/f10
	for C in a b c d e f g h i j k l; do
		DIR=`eval echo \\$DIR$MTPT`
		echo -n $C | dd of=$DIR/f10 bs=1 seek=$OFFSET count=1
		[ "$MTPT" -eq 1 ] && MTPT=2 || MTPT=1
		OFFSET=`expr $OFFSET + 1`
	done
	[ "`cat $DIR1/f10`" = "abcdefghijkl" ] || \
		error "`od -a $DIR1/f10` != abcdefghijkl"
}
run_test 10a "write of file with sub-page size on multiple mounts "

test_10b() {
	yes "R" | dd of=$DIR1/f10b bs=3k count=1 || error "dd $DIR1"

	truncate $DIR1/f10b 4096 || error "truncate 4096"

	dd if=$DIR2/f10b of=$TMP/f10b-lustre bs=4k count=1 || error "dd $DIR2"

	# create a test file locally to compare
	yes "R" | dd of=$TMP/f10b bs=3k count=1 || error "dd random"
	truncate $TMP/f10b 4096 || error "truncate 4096"
	cmp $TMP/f10b $TMP/f10b-lustre || error "file miscompare"
}
run_test 10b "write of file with sub-page size on multiple mounts "

test_11() {
	mkdir $DIR1/d11
	multiop $DIR1/d11/f O_c &
	MULTIPID=$!
	usleep 200
	cp -p /bin/ls $DIR1/d11/f
	$DIR2/d11/f
	RC=$?
	kill -USR1 $MULTIPID
	wait $MULTIPID || error
	[ $RC -eq 0 ] && error || true
}
run_test 11 "execution of file opened for write should return error ===="

test_12() {
       sh lockorder.sh
}
run_test 12 "test lock ordering (link, stat, unlink) ==========="

test_13() {	# bug 2451 - directory coherency
       rm -rf $DIR1/d13
       mkdir $DIR1/d13 || error
       cd $DIR1/d13 || error
       ls
       ( touch $DIR1/d13/f13 ) # needs to be a separate shell
       ls
       rm -f $DIR2/d13/f13 || error
       ls 2>&1 | grep f13 && error "f13 shouldn't return an error (1)" || true
       # need to run it twice
       ( touch $DIR1/d13/f13 ) # needs to be a separate shell
       ls
       rm -f $DIR2/d13/f13 || error
       ls 2>&1 | grep f13 && error "f13 shouldn't return an error (2)" || true
}
run_test 13 "test directory page revocation ===================="

test_14() {
	mkdir $DIR1/d14
	cp -p /bin/ls $DIR1/d14/ls
	exec 100>> $DIR1/d14/ls
	$DIR2/d14/ls && error || true
	exec 100<&-
}
run_test 14 "execution of file open for write returns -ETXTBSY ="

test_14a() {
        mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
        $DIR1/d14/multiop $TMP/test14.junk O_c &
        MULTIPID=$!
        sleep 1
        multiop $DIR2/d14/multiop Oc && error "expected error, got success"
        kill -USR1 $MULTIPID || return 2
        wait $MULTIPID || return 3
}
run_test 14a "open(RDWR) of executing file returns -ETXTBSY ===="

test_14b() { # bug 3192
        mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
        $DIR1/d14/multiop $TMP/test14.junk O_c &
        MULTIPID=$!
        sleep 1
        truncate $DIR2/d14/multiop 0 && error "expected error, got success"
        kill -USR1 $MULTIPID || return 2
        wait $MULTIPID || return 3
}
run_test 14b "truncate of executing file returns -ETXTBSY ======"

test_14c() { # bug 3430
	mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
	$DIR1/d14/multiop $TMP/test14.junk O_c &
	MULTIPID=$!
	sleep 1
	cp /etc/hosts $DIR2/d14/multiop && error "expected error, got success"
	kill -USR1 $MULTIPID || return 2
	wait $MULTIPID || return 3
	#cmp `which multiop` $DIR1/d14/multiop || error "binary changed"
}
run_test 14c "open(O_TRUNC) of executing file return -ETXTBSY =="

test_15() {	# bug 974 - ENOSPC
	echo $PATH
	sh oos2.sh $MOUNT1 $MOUNT2
}
run_test 15 "test out-of-space with multiple writers ==========="

test_16() {
	fsx -c 50 -p 100 -N 2500 $MOUNT1/fsxfile $MOUNT2/fsxfile
}
run_test 16 "2500 iterations of dual-mount fsx ================="

cancel_lru_locks() {
	for d in /proc/fs/lustre/ldlm/namespaces/$1*; do
		echo clear > $d/lru_size
	done
	grep [0-9] /proc/fs/lustre/ldlm/namespaces/$1*/lock_unused_count /dev/null
}

test_17() { # bug 3513, 3667
	[ ! -d /proc/fs/lustre/ost ] && echo "skipping OST-only test" && return

	cp /etc/termcap $DIR1/f17
	cancel_lru_locks OSC > /dev/null
	#define OBD_FAIL_ONCE|OBD_FAIL_LDLM_CREATE_RESOURCE    0x30a
	echo 0x8000030a > /proc/sys/lustre/fail_loc
	ls -ls $DIR1/f17 | awk '{ print $1,$6 }' > $DIR1/f17-1 & \
	ls -ls $DIR2/f17 | awk '{ print $1,$6 }' > $DIR2/f17-2
	wait
	diff -u $DIR1/f17-1 $DIR2/f17-2 || error "files are different"
}
run_test 17 "resource creation/LVB creation race ==============="

test_18() {
	./mmap_sanity -d $MOUNT1 -m $MOUNT2
}
run_test 18 "mmap sanity check ================================="

log "cleanup: ======================================================"
rm -rf $DIR1/[df][0-9]* $DIR1/lnk || true

echo '=========================== finished ==============================='
[ -f "$SANITYLOG" ] && cat $SANITYLOG && exit 1 || true
