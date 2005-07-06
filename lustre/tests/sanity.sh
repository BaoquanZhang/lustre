#!/bin/bash
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# e.g. ONLY="22 23" or ONLY="`seq 32 39`" or EXCEPT="31"
set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped tests:
# skipped test: 
# - 48a is obsolete due to new_kernel_api
# - 51b 51c depend on used kernel
#   more than only LOV EAs
# - 65h (default stripe inheritance) is not implemented for LMV 
#   configurations. Will be done in second phase of collibri.
# - 71 mmap still not updated on HEAD

ALWAYS_EXCEPT=${ALWAYS_EXCEPT:-"24n 48a 51b 51c 65h 71"}
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

[ "$ALWAYS_EXCEPT$EXCEPT" ] && echo "Skipping tests: $ALWAYS_EXCEPT $EXCEPT"

SRCDIR=`dirname $0`
export PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH
export SECURITY=${SECURITY:-"null"}

TMP=${TMP:-/tmp}
DEF_FSTYPE=`test "x$(uname -r | grep -o '2.6')" = "x2.6" && echo "ldiskfs" || echo "ext3"`
FSTYPE=${FSTYPE:-$DEF_FSTYPE}
#used only if FSTYPE == smfs, otherwise ignored by lconf
MDS_BACKFSTYPE=${MDS_BACKFSTYPE:-$DEF_FSTYPE}
OST_BACKFSTYPE=${OST_BACKFSTYPE:-$DEF_FSTYPE}

CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFS=${LFS:-lfs}
LSTRIPE=${LSTRIPE:-"$LFS setstripe"}
LFIND=${LFIND:-"$LFS find"}
LVERIFY=${LVERIFY:-ll_dirstripe_verify}
LCTL=${LCTL:-lctl}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
TOEXCL=${TOEXCL:-toexcl}
TRUNCATE=${TRUNCATE:-truncate}
MUNLINK=${MUNLINK:-munlink}
SOCKETSERVER=${SOCKETSERVER:-socketserver}
SOCKETCLIENT=${SOCKETCLIENT:-socketclient}
IOPENTEST1=${IOPENTEST1:-iopentest1}
IOPENTEST2=${IOPENTEST2:-iopentest2}
MEMHOG=${MEMHOG:-memhog}

. krb5_env.sh

if [ $UID -ne 0 ]; then
	RUNAS_ID="$UID"
	RUNAS=""
else
	RUNAS_ID=${RUNAS_ID:-500}
        if [ -z "$RUNAS_GID" ]; then
	    RUNAS=${RUNAS:-"runas -u $RUNAS_ID"}
        else
	    RUNAS=${RUNAS:-"runas -u $RUNAS_ID -g $RUNAS_GID"}
        fi
fi

if [ `using_krb5_sec $SECURITY` == 'y' ] ; then
    start_krb5_kdc || exit 1
    if [ $RUNAS_ID -ne $UID ]; then
        $RUNAS ./krb5_refresh_cache.sh || exit 2
    fi
fi

export NAME=${NAME:-local}

SAVE_PWD=$PWD

clean() {
	echo -n "cln.."
	sh llmountcleanup.sh > /dev/null || exit 20
	I_MOUNTED=no
}
CLEAN=${CLEAN:-:}

start() {
	echo -n "mnt.."
	sh llrmount.sh > /dev/null || exit 10
	I_MOUNTED=yes
	echo "done"
}
START=${START:-}

log() {
	echo "$*"
	lctl mark "$*" 2> /dev/null || true
}

trace() {
	log "STARTING: $*"
	strace -o $TMP/$1.strace -ttt $*
	RC=$?
	log "FINISHED: $*: rc $RC"
	return 1
}
TRACE=${TRACE:-""}

check_kernel_version() {
	VERSION_FILE=/proc/fs/lustre/kernel_version
	WANT_VER=$1
	[ ! -f $VERSION_FILE ] && echo "can't find kernel version" && return 1
	GOT_VER=`cat $VERSION_FILE`
	[ $GOT_VER -ge $WANT_VER ] && return 0
	log "test needs at least kernel version $WANT_VER, running $GOT_VER"
	return 1
}

run_one() {
	if ! mount | grep -q $DIR; then
		$START
	fi
	BEFORE=`date +%s`
	log "== test $1: $2= `date +%H:%M:%S` ($BEFORE)"
	export TESTNAME=test_$1
	test_$1 || error "test_$1: exit with rc=$?"
	unset TESTNAME
	pass "($((`date +%s` - $BEFORE))s)"
	cd $SAVE_PWD
	$CLEAN
}

build_test_filter() {
        for O in $ONLY; do
            eval ONLY_${O}=true
        done
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
         base=`basetest $1`
         if [ "$ONLY" ]; then
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

[ "$SANITYLOG" ] && rm -f $SANITYLOG || true

error() { 
	log "FAIL: $@"
	if [ "$SANITYLOG" ]; then
		echo "FAIL: $TESTNAME $@" >> $SANITYLOG
	else
		exit 1
	fi
}

pass() { 
	echo PASS $@
}

mounted_lustre_filesystems() {
	awk '($3 ~ "lustre") { print $2 }' /proc/mounts
}
MOUNT="`mounted_lustre_filesystems`"
if [ -z "$MOUNT" ]; then
	sh llmount.sh
	MOUNT="`mounted_lustre_filesystems`"
	[ -z "$MOUNT" ] && error "NAME=$NAME not mounted"
	I_MOUNTED=yes
fi

[ `echo $MOUNT | wc -w` -gt 1 ] && error "NAME=$NAME mounted more than once"

DIR=${DIR:-$MOUNT}
[ -z "`echo $DIR | grep $MOUNT`" ] && echo "$DIR not in $MOUNT" && exit 99

OSTCOUNT=`cat /proc/fs/lustre/llite/fs0/lov/numobd`
STRIPECOUNT=`cat /proc/fs/lustre/llite/fs0/lov/stripecount`
STRIPESIZE=`cat /proc/fs/lustre/llite/fs0/lov/stripesize`
ORIGFREE=`cat /proc/fs/lustre/llite/fs0/lov/kbytesavail`
MAXFREE=${MAXFREE:-$((200000 * $OSTCOUNT))}

[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
[ -f $DIR/d52b/foo ] && chattr -i $DIR/d52b/foo
rm -rf $DIR/[Rdfs][1-9]*

build_test_filter

echo preparing for tests involving mounts
EXT2_DEV=${EXT2_DEV:-/tmp/SANITY.LOOP}
touch $EXT2_DEV
mke2fs -j -F $EXT2_DEV 8000 > /dev/null

umask 077

test_0() {
	touch $DIR/f
	$CHECKSTAT -t file $DIR/f || error
	rm $DIR/f
	$CHECKSTAT -a $DIR/f || error
}
run_test 0 "touch .../f ; rm .../f ============================="

test_0b() {
       chmod 0755 $DIR || error
       $CHECKSTAT -p 0755 $DIR || error
}
run_test 0b "chmod 0755 $DIR ============================="


test_1a() {
	mkdir $DIR/d1
	mkdir $DIR/d1/d2
	$CHECKSTAT -t dir $DIR/d1/d2 || error
}
run_test 1a "mkdir .../d1; mkdir .../d1/d2 ====================="

test_1b() {
	rmdir $DIR/d1/d2
	rmdir $DIR/d1
	$CHECKSTAT -a $DIR/d1 || error
}
run_test 1b "rmdir .../d1/d2; rmdir .../d1 ====================="

test_2a() {
	mkdir $DIR/d2
	touch $DIR/d2/f
	$CHECKSTAT -t file $DIR/d2/f || error
}
run_test 2a "mkdir .../d2; touch .../d2/f ======================"

test_2b() {
	rm -r $DIR/d2
	$CHECKSTAT -a $DIR/d2 || error
}
run_test 2b "rm -r .../d2; checkstat .../d2/f ======================"

test_3a() {
	mkdir $DIR/d3
	$CHECKSTAT -t dir $DIR/d3 || error
}
run_test 3a "mkdir .../d3 ======================================"

test_3b() {
	if [ ! -d $DIR/d3 ]; then
		mkdir $DIR/d3
	fi
	touch $DIR/d3/f
	$CHECKSTAT -t file $DIR/d3/f || error
}
run_test 3b "touch .../d3/f ===================================="

test_3c() {
	rm -r $DIR/d3
	$CHECKSTAT -a $DIR/d3 || error
}
run_test 3c "rm -r .../d3 ======================================"

test_4a() {
	mkdir $DIR/d4
	$CHECKSTAT -t dir $DIR/d4 || error
}
run_test 4a "mkdir .../d4 ======================================"

test_4b() {
	if [ ! -d $DIR/d4 ]; then
		mkdir $DIR/d4
	fi
	mkdir $DIR/d4/d2
	$CHECKSTAT -t dir $DIR/d4/d2 || error
}
run_test 4b "mkdir .../d4/d2 ==================================="

test_5() {
	mkdir $DIR/d5
	mkdir $DIR/d5/d2
	chmod 0707 $DIR/d5/d2
	$CHECKSTAT -t dir -p 0707 $DIR/d5/d2 || error
}
run_test 5 "mkdir .../d5 .../d5/d2; chmod .../d5/d2 ============"

test_6a() {
	touch $DIR/f6a
	chmod 0666 $DIR/f6a || error
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/f6a || error
}
run_test 6a "touch .../f6a; chmod .../f6a ======================"

test_6b() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6b" && return
	if [ ! -f $DIR/f6a ]; then
		touch $DIR/f6a
		chmod 0666 $DIR/f6a
	fi
	$RUNAS chmod 0444 $DIR/f6a && error
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/f6a || error
}
run_test 6b "$RUNAS chmod .../f6a (should return error) =="

test_6c() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6c" && return
	touch $DIR/f6c
	chown $RUNAS_ID $DIR/f6c || error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/f6c || error
}
run_test 6c "touch .../f6c; chown .../f6c ======================"

test_6d() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6d" && return
	if [ ! -f $DIR/f6c ]; then
		touch $DIR/f6c
		chown $RUNAS_ID $DIR/f6c
	fi
	$RUNAS chown $UID $DIR/f6c && error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/f6c || error
}
run_test 6d "$RUNAS chown .../f6c (should return error) =="

test_6e() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6e" && return
	touch $DIR/f6e
	chgrp $RUNAS_ID $DIR/f6e || error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/f6e || error
}
run_test 6e "touch .../f6e; chgrp .../f6e ======================"

test_6f() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6f" && return
	if [ ! -f $DIR/f6e ]; then
		touch $DIR/f6e
		chgrp $RUNAS_ID $DIR/f6e
	fi
	$RUNAS chgrp $UID $DIR/f6e && error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/f6e || error
}
run_test 6f "$RUNAS chgrp .../f6e (should return error) =="

test_6g() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 6g" && return
        mkdir $DIR/d6g || error
        chmod 777 $DIR/d6g || error
        $RUNAS mkdir $DIR/d6g/d || error
        chmod g+s $DIR/d6g/d || error
        mkdir $DIR/d6g/d/subdir
	$CHECKSTAT -g \#$RUNAS_ID $DIR/d6g/d/subdir || error
}
run_test 6g "Is new dir in sgid dir inheriting group?"

test_7a() {
	mkdir $DIR/d7
	$MCREATE $DIR/d7/f
	chmod 0666 $DIR/d7/f
	$CHECKSTAT -t file -p 0666 $DIR/d7/f || error
}
run_test 7a "mkdir .../d7; mcreate .../d7/f; chmod .../d7/f ===="

test_7b() {
	if [ ! -d $DIR/d7 ]; then
		mkdir $DIR/d7
	fi
	$MCREATE $DIR/d7/f2
	echo -n foo > $DIR/d7/f2
	[ "`cat $DIR/d7/f2`" = "foo" ] || error
	$CHECKSTAT -t file -s 3 $DIR/d7/f2 || error
}
run_test 7b "mkdir .../d7; mcreate d7/f2; echo foo > d7/f2 ====="

test_8() {
	mkdir $DIR/d8
	touch $DIR/d8/f
	chmod 0666 $DIR/d8/f
	$CHECKSTAT -t file -p 0666 $DIR/d8/f || error
}
run_test 8 "mkdir .../d8; touch .../d8/f; chmod .../d8/f ======="

test_9() {
	mkdir $DIR/d9
	mkdir $DIR/d9/d2
	mkdir $DIR/d9/d2/d3
	$CHECKSTAT -t dir $DIR/d9/d2/d3 || error
}
run_test 9 "mkdir .../d9 .../d9/d2 .../d9/d2/d3 ================"

test_10() {
	mkdir $DIR/d10
	mkdir $DIR/d10/d2
	touch $DIR/d10/d2/f
	$CHECKSTAT -t file $DIR/d10/d2/f || error
}
run_test 10 "mkdir .../d10 .../d10/d2; touch .../d10/d2/f ======"

test_11() {
	mkdir $DIR/d11
	mkdir $DIR/d11/d2
	chmod 0666 $DIR/d11/d2
	chmod 0705 $DIR/d11/d2
	$CHECKSTAT -t dir -p 0705 $DIR/d11/d2 || error
}
run_test 11 "mkdir .../d11 d11/d2; chmod .../d11/d2 ============"

test_12() {
	mkdir $DIR/d12
	touch $DIR/d12/f
	chmod 0666 $DIR/d12/f
	chmod 0654 $DIR/d12/f
	$CHECKSTAT -t file -p 0654 $DIR/d12/f || error
}
run_test 12 "touch .../d12/f; chmod .../d12/f .../d12/f ========"

test_13() {
	mkdir $DIR/d13
	dd if=/dev/zero of=$DIR/d13/f count=10
	>  $DIR/d13/f
	$CHECKSTAT -t file -s 0 $DIR/d13/f || error
}
run_test 13 "creat .../d13/f; dd .../d13/f; > .../d13/f ========"

test_14() {
	mkdir $DIR/d14
	touch $DIR/d14/f
	rm $DIR/d14/f
	$CHECKSTAT -a $DIR/d14/f || error
}
run_test 14 "touch .../d14/f; rm .../d14/f; rm .../d14/f ======="

test_15() {
	mkdir $DIR/d15
	touch $DIR/d15/f
	mv $DIR/d15/f $DIR/d15/f2
	$CHECKSTAT -t file $DIR/d15/f2 || error
}
run_test 15 "touch .../d15/f; mv .../d15/f .../d15/f2 =========="

test_16() {
	mkdir $DIR/d16
	touch $DIR/d16/f
	rm -rf $DIR/d16/f
	$CHECKSTAT -a $DIR/d16/f || error
}
run_test 16 "touch .../d16/f; rm -rf .../d16/f ================="

test_17a() {
	mkdir -p $DIR/d17
	touch $DIR/d17/f
	ln -s $DIR/d17/f $DIR/d17/l-exist
	ls -l $DIR/d17
	$CHECKSTAT -l $DIR/d17/f $DIR/d17/l-exist || error
	$CHECKSTAT -f -t f $DIR/d17/l-exist || error
	rm -f $DIR/l-exist
	$CHECKSTAT -a $DIR/l-exist || error
}
run_test 17a "symlinks: create, remove (real) =================="

test_17b() {
	mkdir -p $DIR/d17
	ln -s no-such-file $DIR/d17/l-dangle
	ls -l $DIR/d17
	$CHECKSTAT -l no-such-file $DIR/d17/l-dangle || error
	$CHECKSTAT -fa $DIR/d17/l-dangle || error
	rm -f $DIR/l-dangle
	$CHECKSTAT -a $DIR/l-dangle || error
}
run_test 17b "symlinks: create, remove (dangling) =============="

test_17c() { # bug 3440 - don't save failed open RPC for replay
	mkdir -p $DIR/d17
	ln -s foo $DIR/d17/f17c
	cat $DIR/d17/f17c && error "opened non-existent symlink" || true
}
run_test 17c "symlinks: open dangling (should return error) ===="

test_17d() {
	mkdir -p $DIR/d17
	ln -s foo $DIR/d17/f17d
	touch $DIR/d17/f17d || error "creating to new symlink"
}
run_test 17d "symlinks: create dangling ========================"

test_18() {
	touch $DIR/f
	ls $DIR || error
}
run_test 18 "touch .../f ; ls ... =============================="

test_19a() {
	touch $DIR/f19
	ls -l $DIR
	rm $DIR/f19
	$CHECKSTAT -a $DIR/f19 || error
}
run_test 19a "touch .../f19 ; ls -l ... ; rm .../f19 ==========="

test_19b() {
	ls -l $DIR/f19 && error || true
}
run_test 19b "ls -l .../f19 (should return error) =============="

test_19c() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 19c" && return
	$RUNAS touch $DIR/f19 && error || true
}
run_test 19c "$RUNAS touch .../f19 (should return error) =="

test_19d() {
	cat $DIR/f19 && error || true
}
run_test 19d "cat .../f19 (should return error) =============="

test_20() {
	touch $DIR/f
	rm $DIR/f
	log "1 done"
	touch $DIR/f
	rm $DIR/f
	log "2 done"
	touch $DIR/f
	rm $DIR/f
	log "3 done"
	$CHECKSTAT -a $DIR/f || error
}
run_test 20 "touch .../f ; ls -l ... ==========================="

test_21() {
	mkdir $DIR/d21
	[ -f $DIR/d21/dangle ] && rm -f $DIR/d21/dangle
	ln -s dangle $DIR/d21/link
	echo foo >> $DIR/d21/link
	cat $DIR/d21/dangle
	$CHECKSTAT -t link $DIR/d21/link || error
	$CHECKSTAT -f -t file $DIR/d21/link || error
}
run_test 21 "write to dangling link ============================"

test_22() {
	mkdir $DIR/d22
	chown $RUNAS_ID $DIR/d22
	# Tar gets pissy if it can't access $PWD *sigh*
	(cd /tmp;
	$RUNAS tar cf - /etc/hosts /etc/sysconfig/network | \
	$RUNAS tar xfC - $DIR/d22)
	ls -lR $DIR/d22/etc
	$CHECKSTAT -t dir $DIR/d22/etc || error
	$CHECKSTAT -u \#$RUNAS_ID $DIR/d22/etc || error
}
run_test 22 "unpack tar archive as non-root user ==============="

test_23() {
	mkdir $DIR/d23
	$TOEXCL $DIR/d23/f23
	$TOEXCL -e $DIR/d23/f23 || error
}
run_test 23 "O_CREAT|O_EXCL in subdir =========================="

test_24a() {
	echo '== rename sanity =============================================='
	echo '-- same directory rename'
	mkdir $DIR/R1
	touch $DIR/R1/f
	mv $DIR/R1/f $DIR/R1/g
	$CHECKSTAT -t file $DIR/R1/g || error
}
run_test 24a "touch .../R1/f; rename .../R1/f .../R1/g ========="

test_24b() {
	mkdir $DIR/R2
	touch $DIR/R2/{f,g}
	mv $DIR/R2/f $DIR/R2/g
	$CHECKSTAT -a $DIR/R2/f || error
	$CHECKSTAT -t file $DIR/R2/g || error
}
run_test 24b "touch .../R2/{f,g}; rename .../R2/f .../R2/g ====="

test_24c() {
	mkdir $DIR/R3
	mkdir $DIR/R3/f
	mv $DIR/R3/f $DIR/R3/g
	$CHECKSTAT -a $DIR/R3/f || error
	$CHECKSTAT -t dir $DIR/R3/g || error
}
run_test 24c "mkdir .../R3/f; rename .../R3/f .../R3/g ========="

test_24d() {
	mkdir $DIR/R4
	mkdir $DIR/R4/{f,g}
	mrename $DIR/R4/f $DIR/R4/g
	$CHECKSTAT -a $DIR/R4/f || error
	$CHECKSTAT -t dir $DIR/R4/g || error
}
run_test 24d "mkdir .../R4/{f,g}; rename .../R4/f .../R4/g ====="

test_24e() {
	echo '-- cross directory renames --' 
	mkdir $DIR/R5{a,b}
	touch $DIR/R5a/f
	mv $DIR/R5a/f $DIR/R5b/g
	$CHECKSTAT -a $DIR/R5a/f || error
	$CHECKSTAT -t file $DIR/R5b/g || error
}
run_test 24e "touch .../R5a/f; rename .../R5a/f .../R5b/g ======"

test_24f() {
	mkdir $DIR/R6{a,b}
	touch $DIR/R6a/f $DIR/R6b/g
	mv $DIR/R6a/f $DIR/R6b/g
	$CHECKSTAT -a $DIR/R6a/f || error
	$CHECKSTAT -t file $DIR/R6b/g || error
}
run_test 24f "touch .../R6a/f R6b/g; mv .../R6a/f .../R6b/g ===="

test_24g() {
	mkdir $DIR/R7{a,b}
	mkdir $DIR/R7a/d
	mv $DIR/R7a/d $DIR/R7b/e
	$CHECKSTAT -a $DIR/R7a/d || error
	$CHECKSTAT -t dir $DIR/R7b/e || error
}
run_test 24g "mkdir .../R7{a,b}/d; mv .../R7a/d .../R5b/e ======"

test_24h() {
	mkdir $DIR/R8{a,b}
	mkdir $DIR/R8a/d $DIR/R8b/e
	mrename $DIR/R8a/d $DIR/R8b/e
	$CHECKSTAT -a $DIR/R8a/d || error
	$CHECKSTAT -t dir $DIR/R8b/e || error
}
run_test 24h "mkdir .../R8{a,b}/{d,e}; rename .../R8a/d .../R8b/e"

test_24i() {
	echo "-- rename error cases"
	mkdir $DIR/R9
	mkdir $DIR/R9/a
	touch $DIR/R9/f
	mrename $DIR/R9/f $DIR/R9/a
	$CHECKSTAT -t file $DIR/R9/f || error
	$CHECKSTAT -t dir  $DIR/R9/a || error
	$CHECKSTAT -a file $DIR/R9/a/f || error
}
run_test 24i "rename file to dir error: touch f ; mkdir a ; rename f a"

test_24j() {
	mkdir $DIR/R10
	mrename $DIR/R10/f $DIR/R10/g
	$CHECKSTAT -t dir $DIR/R10 || error
	$CHECKSTAT -a $DIR/R10/f || error
	$CHECKSTAT -a $DIR/R10/g || error
}
run_test 24j "source does not exist ============================" 

test_24k() {
	mkdir $DIR/R11a $DIR/R11a/d
	touch $DIR/R11a/f
	mv $DIR/R11a/f $DIR/R11a/d
	$CHECKSTAT -a $DIR/R11a/f || error
	$CHECKSTAT -t file $DIR/R11a/d/f || error
}
run_test 24k "touch .../R11a/f; mv .../R11a/f .../R11a/d ======="

# bug 2429 - rename foo foo foo creates invalid file
test_24l() {
	f="$DIR/f24l"
	multiop $f OcNs || error
}
run_test 24l "Renaming a file to itself ========================"

test_24m() {
	f="$DIR/f24m"
	multiop $f OcLN ${f}2 ${f}2 || error
}
run_test 24m "Renaming a file to a hard link to itself ========="

test_24n() {
    f="$DIR/f24n"
    # this stats the old file after it was renamed, so it should fail
    touch ${f}
    $CHECKSTAT ${f}
    mv ${f} ${f}.rename
    $CHECKSTAT ${f}.rename
    $CHECKSTAT -a ${f}
}
run_test 24n "Statting the old file after renameing (Posix rename 2)"

test_24o() {
	check_kernel_version 37 || return 0
	rename_many -s random -v -n 10 $DIR
}
run_test 24o "rename of files during htree split ==============="

test_24p() {
       mkdir $DIR/R12{a,b}
       DIRINO=`ls -lid $DIR/R12a | awk '{ print $1 }'`
       mrename $DIR/R12a $DIR/R12b
       $CHECKSTAT -a $DIR/R12a || error
       $CHECKSTAT -t dir $DIR/R12b || error
       DIRINO2=`ls -lid $DIR/R12b | awk '{ print $1 }'`
       [ "$DIRINO" = "$DIRINO2" ] || error "R12a $DIRINO != R12b $DIRINO2"
}
run_test 24p "mkdir .../R12{a,b}; rename .../R12a .../R12b"

test_24q() {
       mkdir $DIR/R13{a,b}
       DIRINO=`ls -lid $DIR/R13a | awk '{ print $1 }'`
       multiop $DIR/R13b D_c &
       MULTIPID=$!

       mrename $DIR/R13a $DIR/R13b
       $CHECKSTAT -a $DIR/R13a || error
       $CHECKSTAT -t dir $DIR/R13b || error
       DIRINO2=`ls -lid $DIR/R13b | awk '{ print $1 }'`
       [ "$DIRINO" = "$DIRINO2" ] || error "R13a $DIRINO != R13b $DIRINO2"
       kill -USR1 $MULTIPID
       wait $MULTIPID || error "multiop close failed"
}
run_test 24q "mkdir .../R13{a,b}; open R13b rename R13a R13b ==="

test_24r() { #bug 3789
       mkdir $DIR/R14a $DIR/R14a/b
       mrename $DIR/R14a $DIR/R14a/b && error "rename to subdir worked!"
       $CHECKSTAT -t dir $DIR/R14a || error "$DIR/R14a missing"
       $CHECKSTAT -t dir $DIR/R14a/b || error "$DIR/R14a/b missing"
}
run_test 24r "mkdir .../R14a/b; rename .../R14a .../R14a/b ====="

test_24s() {
       mkdir $DIR/R15a $DIR/R15a/b $DIR/R15a/b/c
       mrename $DIR/R15a $DIR/R15a/b/c && error "rename to sub-subdir worked!"
       $CHECKSTAT -t dir $DIR/R15a || error "$DIR/R15a missing"
       $CHECKSTAT -t dir $DIR/R15a/b/c || error "$DIR/R15a/b/c missing"
}
run_test 24s "mkdir .../R15a/b/c; rename .../R15a .../R15a/b/c ="

test_24t() {
       mkdir $DIR/R16a $DIR/R16a/b $DIR/R16a/b/c
       mrename $DIR/R16a/b/c $DIR/R16a && error "rename to sub-subdir worked!"
       $CHECKSTAT -t dir $DIR/R16a || error "$DIR/R16a missing"
       $CHECKSTAT -t dir $DIR/R16a/b/c || error "$DIR/R16a/b/c missing"
}
run_test 24t "mkdir .../R16a/b/c; rename .../R16a/b/c .../R16a ="

test_24u() {
        rm -rf /tmp/R17*
        mkdir $DIR/R17_dir
        echo "aaa" > $DIR/R17_file
        mv $DIR/R17_dir /tmp/
        $CHECKSTAT -t dir /tmp/R17_dir || error "move dir out 1"
        $CHECKSTAT -a $DIR/R17_dir || error "move dir out 2"
        mv $DIR/R17_file /tmp/
        $CHECKSTAT -t file /tmp/R17_file || error "move file out 1"
        $CHECKSTAT -a $DIR/R17_file || error "move file out 2"

        mv /tmp/R17_dir $DIR/
        $CHECKSTAT -t dir $DIR/R17_dir || error "move dir in 3"
        $CHECKSTAT -a /tmp/R17_dir || error "move dir in 4"
        mv /tmp/R17_file $DIR/
        $CHECKSTAT -t file $DIR/R17_file || error "move file in 3"
        $CHECKSTAT -a /tmp/R17_file || error "move file in 4"
}
run_test 24u "rename across lustre file system"

test_25a() {
	echo '== symlink sanity ============================================='
	mkdir $DIR/d25
	ln -s d25 $DIR/s25
	touch $DIR/s25/foo || error
}
run_test 25a "create file in symlinked directory ==============="

test_25b() {
	[ ! -d $DIR/d25 ] && test_25a
	$CHECKSTAT -t file $DIR/s25/foo || error
}
run_test 25b "lookup file in symlinked directory ==============="

test_26a() {
	mkdir $DIR/d26
	mkdir $DIR/d26/d26-2
	ln -s d26/d26-2 $DIR/s26
	touch $DIR/s26/foo || error
}
run_test 26a "multiple component symlink ======================="

test_26b() {
	mkdir -p $DIR/d26b/d26-2
	ln -s d26b/d26-2/foo $DIR/s26-2
	touch $DIR/s26-2 || error
}
run_test 26b "multiple component symlink at end of lookup ======"

test_26c() {
	mkdir $DIR/d26.2
	touch $DIR/d26.2/foo
	ln -s d26.2 $DIR/s26.2-1
	ln -s s26.2-1 $DIR/s26.2-2
	ln -s s26.2-2 $DIR/s26.2-3
	chmod 0666 $DIR/s26.2-3/foo
}
run_test 26c "chain of symlinks ================================"

# recursive symlinks (bug 439)
test_26d() {
	ln -s d26-3/foo $DIR/d26-3
}
run_test 26d "create multiple component recursive symlink ======"

test_26e() {
	[ ! -h $DIR/d26-3 ] && test_26d
	rm $DIR/d26-3
}
run_test 26e "unlink multiple component recursive symlink ======"

test_27a() {
	echo '== stripe sanity =============================================='
	mkdir $DIR/d27
        $LSTRIPE $DIR/d27/f0 65536 0 1 || error "lstripe failed"
        $CHECKSTAT -t file $DIR/d27/f0 || error "checkstat failed"
	pass
	log "== test_27b: write to one stripe file ========================="
	cp /etc/hosts $DIR/d27/f0 || error
}
run_test 27a "one stripe file =================================="

test_27c() {
	[ "$OSTCOUNT" -lt "2" ] && echo "skipping 2-stripe test" && return
	if [ ! -d $DIR/d27 ]; then
		mkdir $DIR/d27
	fi
	$LSTRIPE $DIR/d27/f01 65536 0 2 || error "lstripe failed"
	[ `$LFIND $DIR/d27/f01 --quiet | grep [^[:blank:]*] | wc -l` -eq 2 ] ||
		error "two-stripe file doesn't have two stripes"
	pass
	log "== test_27d: write to two stripe file file f01 ================"
	dd if=/dev/zero of=$DIR/d27/f01 bs=4k count=4 || error "dd failed"
}
run_test 27c "create two stripe file f01 ======================="

test_27d() {
	if [ ! -d $DIR/d27 ]; then
		mkdir $DIR/d27
	fi
        $LSTRIPE $DIR/d27/fdef 0 -1 0 || error "lstripe failed"
        $CHECKSTAT -t file $DIR/d27/fdef || error "checkstat failed"
	#dd if=/dev/zero of=$DIR/d27/fdef bs=4k count=4 || error
}
run_test 27d "create file with default settings ================"

test_27e() {
	if [ ! -d $DIR/d27 ]; then
		mkdir $DIR/d27
	fi
        $LSTRIPE $DIR/d27/f12 65536 0 2 || error "lstripe failed"
        $LSTRIPE $DIR/d27/f12 65536 0 2 && error "lstripe succeeded twice"
        $CHECKSTAT -t file $DIR/d27/f12 || error "checkstat failed"
}
run_test 27e "lstripe existing file (should return error) ======"

test_27f() {
	if [ ! -d $DIR/d27 ]; then
		mkdir $DIR/d27
	fi
        $LSTRIPE $DIR/d27/fbad 100 0 1 && error "lstripe failed"
        dd if=/dev/zero of=$DIR/d27/f12 bs=4k count=4 || error "dd failed"
        $LFIND $DIR/d27/fbad || error "lfind failed"

}
run_test 27f "lstripe with bad stripe size (should return error)"

test_27g() {
	if [ ! -d $DIR/d27 ]; then
		mkdir $DIR/d27
	fi
	$MCREATE $DIR/d27/fnone || error "mcreate failed"
	pass
	log "== test 27h: lfind with no objects ============================"
	$LFIND $DIR/d27/fnone 2>&1 | grep "no stripe info" || error "has object"
	pass
	log "== test 27i: lfind with some objects =========================="
	touch $DIR/d27/fsome || error "touch failed"
        $LFIND $DIR/d27/fsome | grep obdidx || error "missing objects"

}
run_test 27g "test lfind ======================================="

test_27j() {
        if [ ! -d $DIR/d27 ]; then
                mkdir $DIR/d27
        fi
	$LSTRIPE $DIR/d27/f27j 65536 $OSTCOUNT 1 && error "lstripe failed"||true
}
run_test 27j "lstripe with bad stripe offset (should return error)"

test_27k() { # bug 2844
	FILE=$DIR/d27/f27k
	LL_MAX_BLKSIZE=$((4 * 1024 * 1024))
	[ ! -d $DIR/d27 ] && mkdir -p $DIR/d27
	$LSTRIPE $FILE 67108864 -1 0 || error "lstripe failed"
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
	dd if=/dev/zero of=$FILE bs=4k count=1
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
}
run_test 27k "limit i_blksize for broken user apps ============="

test_27l() {
	mcreate $DIR/f27l || error "creating file"
	$RUNAS $LSTRIPE $DIR/f27l 65536 -1 1 && \
		error "lstripe should have failed" || true
}
run_test 27l "check setstripe permissions (should return error)"

test_27m() {
        [ "$OSTCOUNT" -lt "2" ] && echo "skipping out-of-space test on OST0" && return
        if [ $ORIGFREE -gt $MAXFREE ]; then
                echo "skipping out-of-space test on OST0"
                return
        fi
        mkdir -p $DIR/d27
        $LSTRIPE $DIR/d27/f27m_1 0 0 1
        dd if=/dev/zero of=$DIR/d27/f27m_1 bs=1024 count=$MAXFREE && \
                error "dd should fill OST0"
        i=2
        while $LSTRIPE $DIR/d27/f27m_$i 0 0 1 ; do
                i=`expr $i + 1`
                [ $i -gt 256 ] && break
        done
        i=`expr $i + 1`
        touch $DIR/d27/f27m_$i
        [ `$LFIND $DIR/d27/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] && \
                error "OST0 was full but new created file still use it"
        i=`expr $i + 1`
        touch $DIR/d27/f27m_$i
        [ `$LFIND $DIR/d27/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] && \
                error "OST0 was full but new created file still use it"
        rm $DIR/d27/f27m_1
}
run_test 27m "create file while OST0 was full =================="

test_28() {
	mkdir $DIR/d28
	$CREATETEST $DIR/d28/ct || error
}
run_test 28 "create/mknod/mkdir with bad file types ============"

cancel_lru_locks() {
	for d in /proc/fs/lustre/ldlm/namespaces/$1*; do
		echo clear > $d/lru_size
	done
	grep [0-9] /proc/fs/lustre/ldlm/namespaces/$1*/lock_unused_count /dev/null
}

test_29() {
	cancel_lru_locks MDC
	mkdir $DIR/d29
	touch $DIR/d29/foo
	log 'first d29'
	ls -l $DIR/d29
	MDCDIR=${MDCDIR:-/proc/fs/lustre/ldlm/namespaces/MDC_*}
	LOCKCOUNTORIG=`cat $MDCDIR/lock_count`
	LOCKUNUSEDCOUNTORIG=`cat $MDCDIR/lock_unused_count`
	log 'second d29'
	ls -l $DIR/d29
	log 'done'
	LOCKCOUNTCURRENT=`cat $MDCDIR/lock_count`
	LOCKUNUSEDCOUNTCURRENT=`cat $MDCDIR/lock_unused_count`
	if [ $LOCKCOUNTCURRENT -gt $LOCKCOUNTORIG ]; then
		echo "CURRENT: $LOCKCOUNTCURRENT > $LOCKCOUNTORIG"
		error
	fi
	if [ $LOCKUNUSEDCOUNTCURRENT -gt $LOCKUNUSEDCOUNTORIG ]; then
		echo "UNUSED: $LOCKUNUSEDCOUNTCURRENT > $LOCKUNUSEDCOUNTORIG"
		error
	fi
}
run_test 29 "IT_GETATTR regression  ============================"

test_30() {
	cp `which ls` $DIR
	$DIR/ls /
	rm $DIR/ls
}
run_test 30 "run binary from Lustre (execve) ==================="

test_31a() {
	$OPENUNLINK $DIR/f31 $DIR/f31 || error
	$CHECKSTAT -a $DIR/f31 || error
}
run_test 31a "open-unlink file =================================="

test_31b() {
	touch $DIR/f31 || error
	ln $DIR/f31 $DIR/f31b || error
	multiop $DIR/f31b Ouc || error
	$CHECKSTAT -t file $DIR/f31 || error
}
run_test 31b "unlink file with multiple links while open ======="

test_31c() {
	touch $DIR/f31 || error
	ln $DIR/f31 $DIR/f31c || error
	multiop $DIR/f31 O_uc &
	MULTIPID=$!
	multiop $DIR/f31c Ouc
	usleep 500
	kill -USR1 $MULTIPID
	wait $MULTIPID
}
run_test 31c "open-unlink file with multiple links ============="

test_31d() {
	opendirunlink $DIR/d31d $DIR/d31d || error
	$CHECKSTAT -a $DIR/d31d || error
}
run_test 31d "remove of open directory ========================="

test_31e() { # bug 2904
	check_kernel_version 34 || return 0
	openfilleddirunlink $DIR/d31e || error
}
run_test 31e "remove of open non-empty directory ==============="

test_31f() { # bug 4554
	set -vx
	mkdir $DIR/d31f
	lfs setstripe $DIR/d31f 1048576 -1 1
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	lfs getstripe $DIR/d31f/hosts
	multiop $DIR/d31f D_c &
	MULTIPID=$!

	sleep 1

	rm -rv $DIR/d31f || error "first of $DIR/d31f"
	mkdir $DIR/d31f
	lfs setstripe $DIR/d31f 1048576 -1 1
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	lfs getstripe $DIR/d31f/hosts
	multiop $DIR/d31f D_c &
	MULTIPID2=$!

	sleep 6

	kill -USR1 $MULTIPID || error "first opendir $MULTIPID not running"
	wait $MULTIPID || error "first opendir $MULTIPID failed"

	sleep 6

	kill -USR1 $MULTIPID2 || error "second opendir $MULTIPID not running"
	wait $MULTIPID2 || error "second opendir $MULTIPID2 failed"
	set +vx
}
run_test 31f "remove of open directory with open-unlink file ==="

test_32a() {
	echo "== more mountpoints and symlinks ================="
	[ -e $DIR/d32a ] && rm -fr $DIR/d32a
	mkdir -p $DIR/d32a/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32a/ext2-mountpoint || error
	$CHECKSTAT -t dir $DIR/d32a/ext2-mountpoint/.. || error  
	umount $DIR/d32a/ext2-mountpoint || error
}
run_test 32a "stat d32a/ext2-mountpoint/.. ====================="

test_32b() {
	[ -e $DIR/d32b ] && rm -fr $DIR/d32b
	mkdir -p $DIR/d32b/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32b/ext2-mountpoint || error
	ls -al $DIR/d32b/ext2-mountpoint/.. || error
	umount $DIR/d32b/ext2-mountpoint || error
}
run_test 32b "open d32b/ext2-mountpoint/.. ====================="
 
test_32c() {
	[ -e $DIR/d32c ] && rm -fr $DIR/d32c
	mkdir -p $DIR/d32c/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32c/ext2-mountpoint || error
	mkdir -p $DIR/d32c/d2/test_dir    
	$CHECKSTAT -t dir $DIR/d32c/ext2-mountpoint/../d2/test_dir || error
	umount $DIR/d32c/ext2-mountpoint || error
}
run_test 32c "stat d32c/ext2-mountpoint/../d2/test_dir ========="

test_32d() {
	[ -e $DIR/d32d ] && rm -fr $DIR/d32d
	mkdir -p $DIR/d32d/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32d/ext2-mountpoint || error
	mkdir -p $DIR/d32d/d2/test_dir    
	ls -al $DIR/d32d/ext2-mountpoint/../d2/test_dir || error
	umount $DIR/d32d/ext2-mountpoint || error
}
run_test 32d "open d32d/ext2-mountpoint/../d2/test_dir ========="

test_32e() {
	[ -e $DIR/d32e ] && rm -fr $DIR/d32e
	mkdir -p $DIR/d32e/tmp    
	TMP_DIR=$DIR/d32e/tmp       
	ln -s $DIR/d32e $TMP_DIR/symlink11 
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01 
	$CHECKSTAT -t link $DIR/d32e/tmp/symlink11 || error
	$CHECKSTAT -t link $DIR/d32e/symlink01 || error
}
run_test 32e "stat d32e/symlink->tmp/symlink->lustre-subdir ===="

test_32f() {
	[ -e $DIR/d32f ] && rm -fr $DIR/d32f
	mkdir -p $DIR/d32f/tmp    
	TMP_DIR=$DIR/d32f/tmp       
	ln -s $DIR/d32f $TMP_DIR/symlink11 
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01 
	ls $DIR/d32f/tmp/symlink11  || error
	ls $DIR/d32f/symlink01 || error
}
run_test 32f "open d32f/symlink->tmp/symlink->lustre-subdir ===="

test_32g() {
	[ -e $DIR/d32g ] && rm -fr $DIR/d32g
	[ -e $DIR/test_dir ] && rm -fr $DIR/test_dir
	mkdir -p $DIR/test_dir 
	mkdir -p $DIR/d32g/tmp    
	TMP_DIR=$DIR/d32g/tmp       
	ln -s $DIR/test_dir $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	$CHECKSTAT -t link $DIR/d32g/tmp/symlink12 || error
	$CHECKSTAT -t link $DIR/d32g/symlink02 || error
	$CHECKSTAT -t dir -f $DIR/d32g/tmp/symlink12 || error
	$CHECKSTAT -t dir -f $DIR/d32g/symlink02 || error
}
run_test 32g "stat d32g/symlink->tmp/symlink->lustre-subdir/test_dir"

test_32h() {
	[ -e $DIR/d32h ] && rm -fr $DIR/d32h
	[ -e $DIR/test_dir ] && rm -fr $DIR/test_dir
	mkdir -p $DIR/test_dir 
	mkdir -p $DIR/d32h/tmp    
	TMP_DIR=$DIR/d32h/tmp       
	ln -s $DIR/test_dir $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	ls $DIR/d32h/tmp/symlink12 || error
	ls $DIR/d32h/symlink02  || error
}
run_test 32h "open d32h/symlink->tmp/symlink->lustre-subdir/test_dir"

test_32i() {
	[ -e $DIR/d32i ] && rm -fr $DIR/d32i
	mkdir -p $DIR/d32i/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32i/ext2-mountpoint || error
	touch $DIR/d32i/test_file
	$CHECKSTAT -t file $DIR/d32i/ext2-mountpoint/../test_file || error  
	umount $DIR/d32i/ext2-mountpoint || error
}
run_test 32i "stat d32i/ext2-mountpoint/../test_file ==========="

test_32j() {
	[ -e $DIR/d32j ] && rm -fr $DIR/d32j
	mkdir -p $DIR/d32j/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32j/ext2-mountpoint || error
	touch $DIR/d32j/test_file
	cat $DIR/d32j/ext2-mountpoint/../test_file || error
	umount $DIR/d32j/ext2-mountpoint || error
}
run_test 32j "open d32j/ext2-mountpoint/../test_file ==========="

test_32k() {
	rm -fr $DIR/d32k
	mkdir -p $DIR/d32k/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32k/ext2-mountpoint  
	mkdir -p $DIR/d32k/d2
	touch $DIR/d32k/d2/test_file || error
	$CHECKSTAT -t file $DIR/d32k/ext2-mountpoint/../d2/test_file || error
	umount $DIR/d32k/ext2-mountpoint || error
}
run_test 32k "stat d32k/ext2-mountpoint/../d2/test_file ========"

test_32l() {
	rm -fr $DIR/d32l
	mkdir -p $DIR/d32l/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32l/ext2-mountpoint || error
	mkdir -p $DIR/d32l/d2
	touch $DIR/d32l/d2/test_file
	cat  $DIR/d32l/ext2-mountpoint/../d2/test_file || error
	umount $DIR/d32l/ext2-mountpoint || error
}
run_test 32l "open d32l/ext2-mountpoint/../d2/test_file ========"

test_32m() {
	rm -fr $DIR/d32m
	mkdir -p $DIR/d32m/tmp    
	TMP_DIR=$DIR/d32m/tmp       
	ln -s $DIR $TMP_DIR/symlink11 
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01 
	$CHECKSTAT -t link $DIR/d32m/tmp/symlink11 || error
	$CHECKSTAT -t link $DIR/d32m/symlink01 || error
}
run_test 32m "stat d32m/symlink->tmp/symlink->lustre-root ======"

test_32n() {
	rm -fr $DIR/d32n
	mkdir -p $DIR/d32n/tmp    
	TMP_DIR=$DIR/d32n/tmp       
	ln -s $DIR $TMP_DIR/symlink11 
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01 
	ls -l $DIR/d32n/tmp/symlink11  || error
	ls -l $DIR/d32n/symlink01 || error
}
run_test 32n "open d32n/symlink->tmp/symlink->lustre-root ======"

test_32o() {
	rm -fr $DIR/d32o
	rm -f $DIR/test_file
	touch $DIR/test_file 
	mkdir -p $DIR/d32o/tmp    
	TMP_DIR=$DIR/d32o/tmp       
	ln -s $DIR/test_file $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	$CHECKSTAT -t link $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t link $DIR/d32o/symlink02 || error
	$CHECKSTAT -t file -f $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t file -f $DIR/d32o/symlink02 || error
}
run_test 32o "stat d32o/symlink->tmp/symlink->lustre-root/test_file"

test_32p() {
    log 32p_1
	rm -fr $DIR/d32p
    log 32p_2
	rm -f $DIR/test_file
    log 32p_3
	touch $DIR/test_file 
    log 32p_4
	mkdir -p $DIR/d32p/tmp    
    log 32p_5
	TMP_DIR=$DIR/d32p/tmp       
    log 32p_6
	ln -s $DIR/test_file $TMP_DIR/symlink12 
    log 32p_7
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
    log 32p_8
	cat $DIR/d32p/tmp/symlink12 || error
    log 32p_9
	cat $DIR/d32p/symlink02 || error
    log 32p_10
}
run_test 32p "open d32p/symlink->tmp/symlink->lustre-root/test_file"

test_32q() {
	[ -e $DIR/d32q ] && rm -fr $DIR/d32q
	mkdir -p $DIR/d32q
        touch $DIR/d32q/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32q
	ls $DIR/d32q/under_the_mount && error || true
	umount $DIR/d32q || error
}
run_test 32q "stat follows mountpoints in Lustre (should return error)"

test_32r() {
	[ -e $DIR/d32r ] && rm -fr $DIR/d32r
	mkdir -p $DIR/d32r
        touch $DIR/d32r/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32r
	ls $DIR/d32r | grep -q under_the_mount && error || true
	umount $DIR/d32r || error
}
run_test 32r "opendir follows mountpoints in Lustre (should return error)"

#   chmod 444 /mnt/lustre/somefile
#   open(/mnt/lustre/somefile, O_RDWR)
#   Should return -1
test_33() {
	rm -f $DIR/test_33_file
	touch $DIR/test_33_file
	chmod 444 $DIR/test_33_file
	chown $RUNAS_ID $DIR/test_33_file
        log 33_1
        $RUNAS $OPENFILE -f O_RDWR $DIR/test_33_file && error || true
        log 33_2
}
run_test 33 "write file with mode 444 (should return error) ===="
                                                                                                                                               
test_33a() {
        rm -fr $DIR/d33
        mkdir -p $DIR/d33
        chown $RUNAS_ID $DIR/d33
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33 || error
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33 && error || true
}
run_test 33a "test open file(mode=0444) with O_RDWR (should return error)"

TEST_34_SIZE=${TEST_34_SIZE:-2000000000000}
test_34a() {
	rm -f $DIR/f34
	$MCREATE $DIR/f34 || error
	$LFIND $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$TRUNCATE $DIR/f34 $TEST_34_SIZE || error
	$LFIND $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34a "truncate file that has not been opened ==========="

test_34b() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDONLY $DIR/f34
	$LFIND $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34b "O_RDONLY opening file doesn't create objects ====="

test_34c() {
	[ ! -f $DIR/f34 ] && test_34a 
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDWR $DIR/f34
	$LFIND $DIR/f34 2>&1 | grep -q "no stripe info" && error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34c "O_RDWR opening file-with-size works =============="

test_34d() {
	[ ! -f $DIR/f34 ] && test_34a 
	dd if=/dev/zero of=$DIR/f34 conv=notrunc bs=4k count=1 || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	rm $DIR/f34
}
run_test 34d "write to sparse file ============================="

test_34e() {
	rm -f $DIR/f34e
	$MCREATE $DIR/f34e || error
	$TRUNCATE $DIR/f34e 1000 || error
	$CHECKSTAT -s 1000 $DIR/f34e || error
	$OPENFILE -f O_RDWR $DIR/f34e
	$CHECKSTAT -s 1000 $DIR/f34e || error
}
run_test 34e "create objects, some with size and some without =="

test_35a() {
	cp /bin/sh $DIR/f35a
	chmod 444 $DIR/f35a
	chown $RUNAS_ID $DIR/f35a
	$RUNAS $DIR/f35a && error || true
	rm $DIR/f35a
}
run_test 35a "exec file with mode 444 (should return and not leak) ====="

test_36a() {
	rm -f $DIR/f36
	utime $DIR/f36 || error
}
run_test 36a "MDS utime check (mknod, utime) ==================="

test_36b() {
	echo "" > $DIR/f36
	utime $DIR/f36 || error
}
run_test 36b "OST utime check (open, utime) ===================="

test_36c() {
	rm -f $DIR/d36/f36
	mkdir $DIR/d36
	chown $RUNAS_ID $DIR/d36
	$RUNAS utime $DIR/d36/f36 || error
}
run_test 36c "non-root MDS utime check (mknod, utime) =========="

test_36d() {
	[ ! -d $DIR/d36 ] && test_36c
	echo "" > $DIR/d36/f36
	$RUNAS utime $DIR/d36/f36 || error
}
run_test 36d "non-root OST utime check (open, utime) ==========="

test_36e() {
	[ $RUNAS_ID -eq $UID ] && echo "skipping test 36e" && return
	[ ! -d $DIR/d36 ] && mkdir $DIR/d36
	touch $DIR/d36/f36e
	$RUNAS utime $DIR/d36/f36e && error "utime worked, want failure" || true
}
run_test 36e "utime on non-owned file (should return error) ===="

test_37() {
	mkdir -p $DIR/dextra
	echo f > $DIR/dextra/fbugfile
	mount -t ext2 -o loop $EXT2_DEV $DIR/dextra
	ls $DIR/dextra | grep "\<fbugfile\>" && error
	umount $DIR/dextra || error
	rm -f $DIR/dextra/fbugfile || error
}
run_test 37 "ls a mounted file system to check old content ====="

test_38() {
	o_directory $DIR/test38
}
run_test 38 "open a regular file with O_DIRECTORY =============="

test_39() {
	touch $DIR/test_39_file
	touch $DIR/test_39_file2
#	ls -l  $DIR/test_39_file $DIR/test_39_file2
#	ls -lu  $DIR/test_39_file $DIR/test_39_file2
#	ls -lc  $DIR/test_39_file $DIR/test_39_file2
	sleep 2
	$OPENFILE -f O_CREAT:O_TRUNC:O_WRONLY $DIR/test_39_file2
#	ls -l  $DIR/test_39_file $DIR/test_39_file2
#	ls -lu  $DIR/test_39_file $DIR/test_39_file2
#	ls -lc  $DIR/test_39_file $DIR/test_39_file2
	[ $DIR/test_39_file2 -nt $DIR/test_39_file ] || error
}
run_test 39 "mtime changed on create ==========================="

test_40() {
	dd if=/dev/zero of=$DIR/f40 bs=4096 count=1
	$RUNAS $OPENFILE -f O_WRONLY:O_TRUNC $DIR/f40 && error
	$CHECKSTAT -t file -s 4096 $DIR/f40 || error
}
run_test 40 "failed open(O_TRUNC) doesn't truncate ============="

test_41() {
	# bug 1553
	small_write $DIR/f41 18
}
run_test 41 "test small file write + fstat ====================="

count_ost_writes() {
        cat /proc/fs/lustre/osc/*/stats |
            awk -vwrites=0 '/ost_write/ { writes += $2 } END { print writes; }'
}

# decent default
WRITEBACK_SAVE=500

start_writeback() {
	# in 2.6, restore /proc/sys/vm/dirty_writeback_centisecs
	if [ -f /proc/sys/vm/dirty_writeback_centisecs ]; then
		echo $WRITEBACK_SAVE > /proc/sys/vm/dirty_writeback_centisecs
	else
		# if file not here, we are a 2.4 kernel
		kill -CONT `pidof kupdated`
	fi
}
stop_writeback() {
	# setup the trap first, so someone cannot exit the test at the
	# exact wrong time and mess up a machine
	trap start_writeback EXIT
	# in 2.6, save and 0 /proc/sys/vm/dirty_writeback_centisecs
	if [ -f /proc/sys/vm/dirty_writeback_centisecs ]; then
		WRITEBACK_SAVE=`cat /proc/sys/vm/dirty_writeback_centisecs`
		echo 0 > /proc/sys/vm/dirty_writeback_centisecs
	else
		# if file not here, we are a 2.4 kernel
		kill -STOP `pidof kupdated`
	fi
}

# ensure that all stripes have some grant before we test client-side cache
setup_test42() {
	[ "$SETUP_TEST42" ] && return
	for i in `seq -f $DIR/f42-%g 1 $OSTCOUNT`; do
		dd if=/dev/zero of=$i bs=4k count=1
		rm $i
	done
	SETUP_TEST42=DONE
}

# Tests 42* verify that our behaviour is correct WRT caching, file closure,
# file truncation, and file removal.
test_42a() {
	setup_test42
	cancel_lru_locks OSC
	stop_writeback
	sync; sleep 1; sync # just to be safe
	BEFOREWRITES=`count_ost_writes`
	grep [0-9] /proc/fs/lustre/osc/OSC*MNT*/cur_grant_bytes
	dd if=/dev/zero of=$DIR/f42a bs=1024 count=100
	AFTERWRITES=`count_ost_writes`
	[ $BEFOREWRITES -eq $AFTERWRITES ] || \
		error "$BEFOREWRITES < $AFTERWRITES"
	start_writeback
}
run_test 42a "ensure that we don't flush on close =============="

test_42b() {
	setup_test42
	cancel_lru_locks OSC
	stop_writeback
        sync
        dd if=/dev/zero of=$DIR/f42b bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $MUNLINK $DIR/f42b || error "$MUNLINK $DIR/f42b: $?"
        AFTERWRITES=`count_ost_writes`
        [ $BEFOREWRITES -eq $AFTERWRITES ] ||
            error "$BEFOREWRITES < $AFTERWRITES on unlink"
        BEFOREWRITES=`count_ost_writes`
        sync || error "sync: $?"
        AFTERWRITES=`count_ost_writes`
        [ $BEFOREWRITES -eq $AFTERWRITES ] ||
            error "$BEFOREWRITES < $AFTERWRITES on sync"
        dmesg | grep 'error from obd_brw_async' && error 'error writing back'
	start_writeback
        return 0
}
run_test 42b "test destroy of file with cached dirty data ======"

# if these tests just want to test the effect of truncation,
# they have to be very careful.  consider:
# - the first open gets a {0,EOF}PR lock
# - the first write conflicts and gets a {0, count-1}PW
# - the rest of the writes are under {count,EOF}PW
# - the open for truncate tries to match a {0,EOF}PR
#   for the filesize and cancels the PWs.
# any number of fixes (don't get {0,EOF} on open, match
# composite locks, do smarter file size management) fix
# this, but for now we want these tests to verify that
# the cancellation with truncate intent works, so we
# start the file with a full-file pw lock to match against
# until the truncate.
trunc_test() {
        test=$1
        file=$DIR/$test
        offset=$2
	cancel_lru_locks OSC
	stop_writeback
	# prime the file with 0,EOF PW to match
	touch $file
        $TRUNCATE $file 0
        sync; sync
	# now the real test..
        dd if=/dev/zero of=$file bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $TRUNCATE $file $offset
        cancel_lru_locks OSC
        AFTERWRITES=`count_ost_writes`
	start_writeback
}

test_42c() {
        trunc_test 42c 1024
        [ $BEFOREWRITES -eq $AFTERWRITES ] && \
		error "beforewrites $BEFOREWRITES == afterwrites $AFTERWRITES on truncate"
        rm $file
}
run_test 42c "test partial truncate of file with cached dirty data"

test_42d() {
        trunc_test 42d 0
        [ $BEFOREWRITES -eq $AFTERWRITES ] || \
            error "beforewrites $BEFOREWRITES != afterwrites $AFTERWRITES on truncate"
        rm $file
}
run_test 42d "test complete truncate of file with cached dirty data"

test_43() {
	mkdir $DIR/d43
	cp -p /bin/ls $DIR/d43/f
	exec 100>> $DIR/d43/f
	$DIR/d43/f && error || true
	exec 100<&-
}
run_test 43 "execution of file opened for write should return -ETXTBSY"

test_43a() {
        mkdir -p $DIR/d43
	cp -p `which multiop` $DIR/d43/multiop
        $DIR/d43/multiop $TMP/test43.junk O_c &
        MULTIPID=$!
        sleep 1
        multiop $DIR/d43/multiop Oc && error "expected error, got success"
        kill -USR1 $MULTIPID || return 2
        wait $MULTIPID || return 3
}
run_test 43a "open(RDWR) of file being executed should return -ETXTBSY"

test_43b() {
        mkdir -p $DIR/d43
	cp -p `which multiop` $DIR/d43/multiop
        $DIR/d43/multiop $TMP/test43.junk O_c &
        MULTIPID=$!
        sleep 1
        truncate $DIR/d43/multiop 0 && error "expected error, got success"
        kill -USR1 $MULTIPID || return 2
        wait $MULTIPID || return 3
}
run_test 43b "truncate of file being executed should return -ETXTBSY"

test_43c() {
	local testdir="$DIR/d43c"
	mkdir -p $testdir
	cp $SHELL $testdir/
	( cd $(dirname $SHELL) && md5sum $(basename $SHELL) ) | \
		( cd $testdir && md5sum -c)
}
run_test 43c "md5sum of copy into lustre========================"

test_44() {
	[  "$OSTCOUNT" -lt "2" ] && echo "skipping 2-stripe test" && return
	dd if=/dev/zero of=$DIR/f1 bs=4k count=1 seek=1023
	dd if=$DIR/f1 bs=4k count=1
}
run_test 44 "zero length read from a sparse stripe ============="

test_44a() {
    local nstripe=`$LCTL lov_getconfig $DIR | grep default_stripe_count: | \
                         awk '{print $2}'`
    local stride=`$LCTL lov_getconfig $DIR | grep default_stripe_size: | \
                      awk '{print $2}'`
    if [ $nstripe -eq 0 ] ; then
        nstripe=`$LCTL lov_getconfig $DIR | grep obd_count: | awk '{print $2}'`
    fi

    OFFSETS="0 $((stride/2)) $((stride-1))"
    for offset in $OFFSETS ; do
      for i in `seq 0 $((nstripe-1))`; do
        rm -f $DIR/d44a
        local GLOBALOFFSETS=""
        local size=$((((i + 2 * $nstripe )*$stride + $offset)))  # Bytes
        ll_sparseness_write $DIR/d44a $size  || error "ll_sparseness_write"
        GLOBALOFFSETS="$GLOBALOFFSETS $size"
        ll_sparseness_verify $DIR/d44a $GLOBALOFFSETS \
                            || error "ll_sparseness_verify $GLOBALOFFSETS"

        for j in `seq 0 $((nstripe-1))`; do
            size=$((((j + $nstripe )*$stride + $offset)))  # Bytes
            ll_sparseness_write $DIR/d44a $size || error "ll_sparseness_write"
            GLOBALOFFSETS="$GLOBALOFFSETS $size"
        done
        ll_sparseness_verify $DIR/d44a $GLOBALOFFSETS \
                            || error "ll_sparseness_verify $GLOBALOFFSETS"
      done
    done
}
run_test 44a "test sparse pwrite ==============================="

dirty_osc_total() {
	tot=0
	for d in /proc/fs/lustre/osc/*/cur_dirty_bytes; do
		tot=$(($tot + `cat $d`))
	done
	echo $tot
}
do_dirty_record() {
	before=`dirty_osc_total`
	echo executing "\"$*\""
	eval $*
	after=`dirty_osc_total`
	echo before $before, after $after
}
test_45() {
	f="$DIR/f45"
	# Obtain grants from OST if it supports it
	echo blah > ${f}_grant
	stop_writeback
	sync
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "> $f"
	[ $before -gt $after ] || error "truncate didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "sync"
	[ $before -gt $after ] || error "writeback didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "cancel_lru_locks OSC"
	[ $before -gt $after ] || error "lock cancellation didn't lower dirty count"
	start_writeback
}
run_test 45 "osc io page accounting ============================"

page_size() {
	getconf PAGE_SIZE
}

# in a 2 stripe file (lov.sh), page 1023 maps to page 511 in its object.  this
# test tickles a bug where re-dirtying a page was failing to be mapped to the
# objects offset and an assert hit when an rpc was built with 1023's mapped 
# offset 511 and 511's raw 511 offset. it also found general redirtying bugs.
test_46() {
	f="$DIR/f46"
	stop_writeback
	sync
	dd if=/dev/zero of=$f bs=`page_size` seek=511 count=1
	sync
	dd conv=notrunc if=/dev/zero of=$f bs=`page_size` seek=1023 count=1
	dd conv=notrunc if=/dev/zero of=$f bs=`page_size` seek=511 count=1
	sync
	start_writeback
}
run_test 46 "dirtying a previously written page ================"

# Check that device nodes are created and then visible correctly (#2091)
test_47() {
	cmknod $DIR/test_47_node || error
}
run_test 47 "Device nodes check ================================"

test_48a() { # bug 2399
	check_kernel_version 34 || return 0
	mkdir -p $DIR/d48a
	cd $DIR/d48a
	mv $DIR/d48a $DIR/d48.new || error "move directory failed"
	mkdir $DIR/d48a || error "recreate directory failed"
	touch foo || error "'touch foo' failed after recreating cwd"
	mkdir bar || error "'mkdir foo' failed after recreating cwd"
	ls . || error "'ls .' failed after recreating cwd"
	ls .. || error "'ls ..' failed after removing cwd"
	cd . || error "'cd .' failed after recreating cwd"
	mkdir . && error "'mkdir .' worked after recreating cwd"
	rmdir . && error "'rmdir .' worked after recreating cwd"
	ln -s . baz || error "'ln -s .' failed after recreating cwd"
	cd .. || error "'cd ..' failed after recreating cwd"
}
run_test 48a "Access renamed working dir (should return errors)="

test_48b() { # bug 2399
	check_kernel_version 34 || return 0
	mkdir -p $DIR/d48b
	cd $DIR/d48b
	rmdir $DIR/d48b || error "remove cwd $DIR/d48b failed"
	touch foo && error "'touch foo' worked after removing cwd"
	mkdir foo && error "'mkdir foo' worked after removing cwd"
	ls . && error "'ls .' worked after removing cwd"
	ls .. || error "'ls ..' failed after removing cwd"
	cd . && error "'cd .' worked after removing cwd"
	mkdir . && error "'mkdir .' worked after removing cwd"
	rmdir . && error "'rmdir .' worked after removing cwd"
        ln -s . foo && error "'ln -s .' worked after removing cwd"
        cd .. || echo "'cd ..' failed after removing cwd `pwd`"  #bug 3517

}
run_test 48b "Access removed working dir (should return errors)="

test_48c() { # bug 2350
	check_kernel_version 36 || return 0
	#sysctl -w portals.debug=-1
	#set -vx
	mkdir -p $DIR/d48c/dir
	cd $DIR/d48c/dir
	$TRACE rmdir $DIR/d48c/dir || error "remove cwd $DIR/d48c/dir failed"
	$TRACE touch foo && error "'touch foo' worked after removing cwd"
	$TRACE mkdir foo && error "'mkdir foo' worked after removing cwd"
	$TRACE ls . && error "'ls .' worked after removing cwd"
	$TRACE ls .. || error "'ls ..' failed after removing cwd"
	$TRACE cd . && error "'cd .' worked after removing cwd"
	$TRACE mkdir . && error "'mkdir .' worked after removing cwd"
	$TRACE rmdir . && error "'rmdir .' worked after removing cwd"
        $TRACE ln -s . foo && error "'ln -s .' worked after removing cwd"
        $TRACE cd .. || echo "'cd ..' failed after removing cwd `pwd`" #bug 3415
}
run_test 48c "Access removed working subdir (should return errors)"

test_48d() { # bug 2350
	check_kernel_version 36 || return 0
	#sysctl -w portals.debug=-1
	#set -vx
	mkdir -p $DIR/d48d/dir
	cd $DIR/d48d/dir
	$TRACE rmdir $DIR/d48d/dir || error "remove cwd $DIR/d48d/dir failed"
	$TRACE rmdir $DIR/d48d || error "remove parent $DIR/d48d failed"
	$TRACE touch foo && error "'touch foo' worked after removing parent"
	$TRACE mkdir foo && error "'mkdir foo' worked after removing parent"
	$TRACE ls . && error "'ls .' worked after removing parent"
	$TRACE ls .. && error "'ls ..' worked after removing parent"
	$TRACE cd . && error "'cd .' worked after recreate parent"
	$TRACE mkdir . && error "'mkdir .' worked after removing parent"
	$TRACE rmdir . && error "'rmdir .' worked after removing parent"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing parent"
	$TRACE cd .. && error "'cd ..' worked after removing parent" || true
}
run_test 48d "Access removed parent subdir (should return errors)"

test_48e() { # bug 4134
	check_kernel_version 41 || return 0
	#sysctl -w portals.debug=-1
	#set -vx
	mkdir -p $DIR/d48e/dir
	# On a buggy kernel addition of "; touch file" after cd .. will
	# produce kernel oops in lookup_hash_it

	cd $DIR/d48e/dir
	( sleep 2 && cd -P .. ) &
	cdpid=$!
	$TRACE rmdir $DIR/d48e/dir || error "remove cwd $DIR/d48e/dir failed"
	$TRACE rmdir $DIR/d48e || error "remove parent $DIR/d48e failed"
	$TRACE touch $DIR/d48e || error "'touch $DIR/d48e' failed"
	$TRACE chmod +x $DIR/d48e || error "'chmod +x $DIR/d48e' failed"
	$TRACE wait $cdpid && error "'cd ..' worked after recreate parent"
	$TRACE rm $DIR/d48e || error "'$DIR/d48e' failed"
}
run_test 48e "Access to recreated parent (should return errors) "

test_50() {
	# bug 1485
	mkdir $DIR/d50
	cd $DIR/d50
	ls /proc/$$/cwd || error
}
run_test 50 "special situations: /proc symlinks  ==============="

test_51() {
	# bug 1516 - create an empty entry right after ".." then split dir
	mkdir $DIR/d49
	touch $DIR/d49/foo
	$MCREATE $DIR/d49/bar
	rm $DIR/d49/foo
	createmany -m $DIR/d49/longfile 201
	FNUM=202
	while [ `ls -sd $DIR/d49 | awk '{ print $1 }'` -eq 4 ]; do
		$MCREATE $DIR/d49/longfile$FNUM
		FNUM=$(($FNUM + 1))
		echo -n "+"
	done
	ls -l $DIR/d49 > /dev/null || error
}
run_test 51 "special situations: split htree with empty entry =="

export NUMTEST=70000
test_51b() {
	NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
	[ $NUMFREE -lt 21000 ] && \
		echo "skipping test 51b, not enough free inodes($NUMFREE)" && \
		return

	check_kernel_version 40 || NUMTEST=31000
	[ $NUMFREE -lt $NUMTEST ] && NUMTEST=$(($NUMFREE - 50))

	mkdir -p $DIR/d51b
	(cd $DIR/d51b; mkdirmany t $NUMTEST)
}
run_test 51b "mkdir .../t-0 --- .../t-$NUMTEST ===================="

test_51c() {
       NUMTEST=70000
       check_kernel_version 40 || NUMTEST=31000
       NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
       [ $NUMFREE -lt $NUMTEST ] && echo "skipping test 51c" && return
       mkdir -p $DIR/d51b
       (cd $DIR/d51b; rmdirmany t $NUMTEST)
}
run_test 51c "rmdir .../t-0 --- .../t-$NUMTEST ===================="

test_52a() {
	[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
	mkdir -p $DIR/d52a
	touch $DIR/d52a/foo
	chattr =a $DIR/d52a/foo || error
	echo bar >> $DIR/d52a/foo || error
	cp /etc/hosts $DIR/d52a/foo && error
	rm -f $DIR/d52a/foo 2>/dev/null && error
	link $DIR/d52a/foo $DIR/d52a/foo_link 2>/dev/null && error
	echo foo >> $DIR/d52a/foo || error
	mrename $DIR/d52a/foo $DIR/d52a/foo_ren && error
	lsattr $DIR/d52a/foo | egrep -q "^-+a-+ $DIR/d52a/foo" || error
	chattr -a $DIR/d52a/foo || error

	rm -fr $DIR/d52a || error
}
run_test 52a "append-only flag test (should return errors) ====="

test_52b() {
	[ -f $DIR/d52b/foo ] && chattr -i $DIR/d52b/foo
	mkdir -p $DIR/d52b
	touch $DIR/d52b/foo
	chattr =i $DIR/d52b/foo || error
	cat test > $DIR/d52b/foo && error
	cp /etc/hosts $DIR/d52b/foo && error
	rm -f $DIR/d52b/foo 2>/dev/null && error
	link $DIR/d52b/foo $DIR/d52b/foo_link 2>/dev/null && error
	echo foo >> $DIR/d52b/foo && error
	mrename $DIR/d52b/foo $DIR/d52b/foo_ren && error
	[ -f $DIR/d52b/foo ] || error
	[ -f $DIR/d52b/foo_ren ] && error
	lsattr $DIR/d52b/foo | egrep -q "^-+i-+ $DIR/d52b/foo" || error
	chattr -i $DIR/d52b/foo || error

	rm -fr $DIR/d52b || error
}
run_test 52b "immutable flag test (should return errors) ======="

test_53() {
        for i in `ls -d /proc/fs/lustre/osc/OSC*mds1 2> /dev/null` ; do
                ostname=`echo $i | cut -d _ -f 3-4 | sed -e s/_mds1//`
                ost_last=`cat /proc/fs/lustre/obdfilter/$ostname/last_id`
                mds_last=`cat $i/prealloc_last_id`
                echo "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                if [ $ost_last != $mds_last ]; then
                    error "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                fi
        done
}
run_test 53 "verify that MDS and OSTs agree on pre-creation ===="

test_54a() {
     	$SOCKETSERVER $DIR/socket
     	$SOCKETCLIENT $DIR/socket || error
      	$MUNLINK $DIR/socket
}
run_test 54a "unix domain socket test =========================="

test_54b() {
	f="$DIR/f54b"
	mknod $f c 1 3
	chmod 0666 $f
	dd if=/dev/zero of=$f bs=`page_size` count=1 
}
run_test 54b "char device works in lustre ======================"

find_loop_dev() {
       [ "$LOOPNUM" ] && return
       [ -b /dev/loop/0 ] && LOOPBASE=/dev/loop/
       [ -b /dev/loop0 ] && LOOPBASE=/dev/loop
       [ -z "$LOOPBASE" ] && echo "/dev/loop/0 and /dev/loop0 gone?" && return

       for i in `seq 3 7`; do
               losetup $LOOPBASE$i > /dev/null 2>&1 && continue
               LOOPDEV=$LOOPBASE$i
               LOOPNUM=$i
               break
       done
}

test_54c() {
	tfile="$DIR/f54c"
	tdir="$DIR/d54c"
	loopdev="$DIR/loop54c"
	
        find_loop_dev
        [ -z "$LOOPNUM" ] && echo "couldn't find empty loop device" && return
        mknod $loopdev b 7 $LOOPNUM
        echo "make a loop file system with $tfile on $loopdev ($LOOPNUM)..."
	
	dd if=/dev/zero of=$tfile bs=`page_size` seek=1024 count=1 > /dev/null
	losetup $loopdev $tfile || error "can't set up $loopdev for $tfile"
	mkfs.ext2 $loopdev || error "mke2fs on $loopdev"
	mkdir -p $tdir
	mount -t ext2 $loopdev $tdir || error "error mounting $loopdev on $tdir"
	dd if=/dev/zero of=$tdir/tmp bs=`page_size` count=30 || error "dd write"
	df $tdir
	dd if=$tdir/tmp of=/dev/zero bs=`page_size` count=30 || error "dd read"
	umount $tdir
	losetup -d $loopdev
	rm $loopdev
}
run_test 54c "block device works in lustre ====================="

test_54d() {
	f="$DIR/f54d"
	string="aaaaaa"
	mknod $f p
	[ "$string" = `echo $string > $f | cat $f` ] || error
}
run_test 54d "fifo device works in lustre ======================"

check_fstype() {
        test "x$FSTYPE" = "x$(grep $FSTYPE /proc/filesystems)" && return 0
        modprobe $FSTYPE > /dev/null 2>&1
        test "x$FSTYPE" = "x$(grep $FSTYPE /proc/filesystems)" && return 0
        test "x$(uname -r | grep -o '2.6')" = "x2.6" && MODEXT="ko" || MODEXT="o"
        insmod ../$FSTYPE/$FSTYPE.$MODEXT > /dev/null 2>&1
        test "x$FSTYPE" = "x$(grep $FSTYPE /proc/filesystems)" && return 0
        return 1
}

test_55() {
        rm -rf $DIR/d55
        mkdir $DIR/d55
        check_fstype && echo "can't find fs $FSTYPE, skipping test 55" && return
        if [ "$FSTYPE" == "smfs" ] ; then
	    mount -t $MDS_BACKFSTYPE -o loop,iopen $EXT2_DEV $DIR/d55 || error "mounting"
	else
	    mount -t $FSTYPE -o loop,iopen $EXT2_DEV $DIR/d55 || error "mounting"
	fi
	touch $DIR/d55/foo
        $IOPENTEST1 $DIR/d55/foo $DIR/d55 || error "running $IOPENTEST1"
        $IOPENTEST2 $DIR/d55 || error "running $IOPENTEST2"
	echo "check for $EXT2_DEV. Please wait..."
        rm -rf $DIR/d55/*
        umount $DIR/d55 || error "unmounting"
}
run_test 55 "check iopen_connect_dentry() ======================"

test_56() {
        rm -rf $DIR/d56
        mkdir $DIR/d56
        mkdir $DIR/d56/dir
        NUMFILES=3
        NUMFILESx2=$(($NUMFILES * 2))
        for i in `seq 1 $NUMFILES` ; do
                touch $DIR/d56/file$i
                touch $DIR/d56/dir/file$i
        done

        # test lfs find with --recursive
        FILENUM=`$LFIND --recursive $DIR/d56 | grep -v OBDS | grep -c obdidx`
        [ $FILENUM -eq $NUMFILESx2 ] || error \
                "lfs find --recursive $DIR/d56 wrong: found $FILENUM, expected $NUMFILESx2"
        FILENUM=`$LFIND $DIR/d56 | grep -v OBDS | grep -c obdidx`
        [ $FILENUM -eq $NUMFILES ] || error \
                "lfs find $DIR/d56 without --recursive wrong: found $FILENUM,
		expected $NUMFILES"
        echo "lfs find --recursive passed."

        # test lfs find with file instead of dir
        FILENUM=`$LFIND $DIR/d56/file1 | grep -v OBDS | grep -c obdidx`
        [ $FILENUM  -eq 1 ] || error \
                 "lfs find $DIR/d56/file1 wrong: found $FILENUM, expected 1"
        echo "lfs find file passed."

        #test lfs find with --verbose
        [ `$LFIND --verbose $DIR/d56 | grep -v OBDS | grep -c lmm_magic` -eq $NUMFILES ] ||\
                error "lfs find --verbose $DIR/d56 wrong: should find $NUMFILES lmm_magic info"
        [ `$LFIND $DIR/d56 | grep -v OBDS | grep -c lmm_magic` -eq 0 ] || error \
                "lfs find $DIR/d56 without --verbose wrong: should not show lmm_magic info"
        echo "lfs find --verbose passed."

        #test lfs find with --obd
        $LFIND --obd wrong_uuid $DIR/d56 2>&1 | grep -q "unknown obduuid" || \
                error "lfs find --obd wrong_uuid should return error information"

        [  "$OSTCOUNT" -lt 2 ] && \
                echo "skipping other lfs find --obd test" && return
        FILENUM=`$LFIND --recursive $DIR/d56 | grep -v OBDS | grep obdidx | wc -l`
        OBDUUID=`$LFIND --recursive $DIR/d56 | grep -A 1 OBDS | grep -v OBDS | awk '{print $3}'`
        OBDIDX=`$LFIND --recursive $DIR/d56 | grep -A 1 OBDS | grep -v OBDS | awk '{print $1}'`
        FOUND=`$LFIND --recursive --obd $OBDUUID $DIR/d56 | wc -l`

        [ $FOUND -eq $FILENUM ] || \
                error "lfs find --obd wrong: found $FOUND, expected $FILENUM"

        [ `$LFIND -r -v --obd $OBDUUID $DIR/d56 | grep "^[[:blank:]]*$OBDIDX" | wc -l` ] || \
                error "lfs find --obd wrong: should not show file on other obd"

        echo "lfs find --obd passed."
}
run_test 56 "check lfs find ===================================="

test_57a() {
	# note test will not do anything if MDS is not local
	for DEV in `cat /proc/fs/lustre/mds/*/mntdev`; do
		dumpe2fs -hf $DEV > $TMP/t57a.dump || error "can't access $DEV"
		DEVISIZE=`awk '/Inode size:/ { print $3 }' $TMP/t57a.dump`
		[ "$DEVISIZE" -gt 128 ] || error "inode size $DEVISIZE"
		rm $TMP/t57a.dump
	done
}
run_test 57a "verify MDS filesystem created with large inodes =="

test_57b() {
	FILECOUNT=100
	FILE1=$DIR/d57b/f1
	FILEN=$DIR/d57b/f$FILECOUNT
	rm -rf $DIR/d57b || error "removing $DIR/d57b"
	mkdir -p $DIR/d57b || error "creating $DIR/d57b"
	echo "mcreating $FILECOUNT files"
	createmany -m $DIR/d57b/f 1 $FILECOUNT || \
		error "creating files in $DIR/d57b"

	# verify that files do not have EAs yet
	$LFIND $FILE1 2>&1 | grep -q "no stripe" || error "$FILE1 has an EA"
	$LFIND $FILEN 2>&1 | grep -q "no stripe" || error "$FILEN has an EA"

	MDSFREE="`cat /proc/fs/lustre/mds/*/kbytesfree`"
	MDCFREE="`cat /proc/fs/lustre/mdc/*/kbytesfree`"
	echo "opening files to create objects/EAs"
	for FILE in `seq -f $DIR/d57b/f%g 1 $FILECOUNT`; do
		$OPENFILE -f O_RDWR $FILE > /dev/null || error "opening $FILE"
	done

	# verify that files have EAs now
	$LFIND $FILE1 | grep -q "obdidx" || error "$FILE1 missing EA"
	$LFIND $FILEN | grep -q "obdidx" || error "$FILEN missing EA"

	MDSFREE2="`cat /proc/fs/lustre/mds/*/kbytesfree`"
	MDCFREE2="`cat /proc/fs/lustre/mdc/*/kbytesfree`"
	
	if [ "$MDCFREE" != "$MDCFREE2" ]; then
		if [ "$MDSFREE" != "$MDSFREE2" ]; then
			error "MDC before $MDCFREE != after $MDCFREE2"
		else
			echo "MDC before $MDCFREE != after $MDCFREE2"
			echo "unable to confirm if MDS has large inodes"
		fi
	fi
	rm -rf $DIR/d57b
}
run_test 57b "default LOV EAs are stored inside large inodes ==="

test_58() {
	wiretest
}
run_test 58 "verify cross-platform wire constants =============="

test_59() {
	echo "touch 130 files"
	for i in `seq 1 130` ; do
		touch $DIR/59-$i
	done
	echo "rm 130 files"
	for i in `seq 1 130` ; do
		rm -f $DIR/59-$i
	done
	sync
	sleep 2
        # wait for commitment of removal
}
run_test 59 "verify cancellation of llog records async ========="

test_60() {
	echo 60 "llog tests run from kernel mode"
	sh run-llog.sh
}
run_test 60 "llog sanity tests run from kernel module =========="

test_61() {
	f="$DIR/f61"
	dd if=/dev/zero of=$f bs=`page_size` count=1
	cancel_lru_locks OSC
	multiop $f OSMWUc || error
	sync
}
run_test 61 "mmap() writes don't make sync hang ================"

# bug 2330 - insufficient obd_match error checking causes LBUG
test_62() {
        f="$DIR/f62"
        echo foo > $f
        cancel_lru_locks OSC
        echo 0x405 > /proc/sys/lustre/fail_loc
        cat $f && error "cat succeeded, expect -EIO"
        echo 0 > /proc/sys/lustre/fail_loc
}
run_test 62 "verify obd_match failure doesn't LBUG (should -EIO)"

# bug 2319 - oig_wait() interrupted causes crash because of invalid waitq.
test_63() {
	MAX_DIRTY_MB=`cat /proc/fs/lustre/osc/*/max_dirty_mb | head -n 1`
	for i in /proc/fs/lustre/osc/*/max_dirty_mb ; do
		echo 0 > $i
	done
	for i in `seq 10` ; do
		dd if=/dev/zero of=$DIR/f63 bs=8k &
		sleep 5
		kill $!
		sleep 1
	done

	for i in /proc/fs/lustre/osc/*/max_dirty_mb ; do
		echo $MAX_DIRTY_MB > $i
	done
	true
}
run_test 63 "Verify oig_wait interruption does not crash ======"

test_64a () {
	df $DIR
	grep "[0-9]" /proc/fs/lustre/osc/OSC*MNT*/cur*
}
run_test 64a "verify filter grant calculations (in kernel) ====="

test_64b () {
	sh oos.sh $MOUNT
}
run_test 64b "check out-of-space detection on client ==========="

# bug 1414 - set/get directories' stripe info
test_65a() {
	mkdir -p $DIR/d65a
	touch $DIR/d65a/f1
	$LVERIFY $DIR/d65a $DIR/d65a/f1 || error "lverify failed"
}
run_test 65a "directory with no stripe info ===================="

test_65b() {
	mkdir -p $DIR/d65b
       	$LSTRIPE $DIR/d65b $(($STRIPESIZE * 2)) 0 1 || error "setstripe"
	touch $DIR/d65b/f2
	$LVERIFY $DIR/d65b $DIR/d65b/f2 || error "lverify failed"
}
run_test 65b "directory setstripe $(($STRIPESIZE * 2)) 0 1 ==============="

test_65c() {
	if [ $OSTCOUNT -gt 1 ]; then
		mkdir -p $DIR/d65c
    		$LSTRIPE $DIR/d65c $(($STRIPESIZE * 4)) 1 \
			$(($OSTCOUNT - 1)) || error "setstripe"
	        touch $DIR/d65c/f3
	        $LVERIFY $DIR/d65c $DIR/d65c/f3 || error "lverify failed"
	fi
}
run_test 65c "directory setstripe $(($STRIPESIZE * 4)) 1 $(($OSTCOUNT - 1))"

[ $STRIPECOUNT -eq 0 ] && sc=1 || sc=$(($STRIPECOUNT - 1))

test_65d() {
	mkdir -p $DIR/d65d
	$LSTRIPE $DIR/d65d $STRIPESIZE -1 $sc || error "setstripe"
	touch $DIR/d65d/f4 $DIR/d65d/f5
	$LVERIFY $DIR/d65d $DIR/d65d/f4 $DIR/d65d/f5 || error "lverify failed"
}
run_test 65d "directory setstripe $STRIPESIZE -1 $sc ======================"

test_65e() {
	mkdir -p $DIR/d65e

	$LSTRIPE $DIR/d65e 0 -1 0 || error "setstripe"
        $LFS find -v $DIR/d65e | grep "$DIR/d65e/ has no stripe info" || error "no stripe info failed"
	touch $DIR/d65e/f6
	$LVERIFY $DIR/d65e $DIR/d65e/f6 || error "lverify failed"
}
run_test 65e "directory setstripe 0 -1 0 (default) ============="

test_65f() {
	mkdir -p $DIR/d65f
	$RUNAS $LSTRIPE $DIR/d65f 0 -1 0 && error "setstripe succeeded" || true
}
run_test 65f "dir setstripe permission (should return error) ==="

test_65g() {
        mkdir -p $DIR/d65g
        $LSTRIPE $DIR/d65g $(($STRIPESIZE * 2)) 0 1 || error "setstripe"
        $LSTRIPE -d $DIR/d65g || error "deleting stripe info failed"
        $LFS find -v $DIR/d65g | grep "$DIR/d65g/ has no stripe info" || error "no stripe info failed"
}
run_test 65g "directory setstripe -d ========"
                                                                                                               
test_65h() {
        mkdir -p $DIR/d65h
        $LSTRIPE $DIR/d65h $(($STRIPESIZE * 2)) 0 1 || error "setstripe"
        mkdir -p $DIR/d65h/dd1
        [ "`$LFS find -v $DIR/d65h | grep "^count"`" == \
          "`$LFS find -v $DIR/d65h/dd1 | grep "^count"`" ] || error "stripe info inherit failed"
}
run_test 65h "directory stripe info inherit ======"

# bug 2543 - update blocks count on client
test_66() {
	COUNT=${COUNT:-8}
	dd if=/dev/zero of=$DIR/f66 bs=1k count=$COUNT
	sync
	BLOCKS=`ls -s $DIR/f66 | awk '{ print $1 }'`
	[ $BLOCKS -ge $COUNT ] || error "$DIR/f66 blocks $BLOCKS < $COUNT"
}
run_test 66 "update inode blocks count on client ==============="

test_67() { # bug 3285 - supplementary group fails on MDS, passes on client
	[ "$RUNAS_ID" = "$UID" ] && echo "skipping test 67" && return
	check_kernel_version 35 || return 0
	mkdir $DIR/d67
	chmod 771 $DIR/d67
	chgrp $RUNAS_ID $DIR/d67
	$RUNAS -g $(($RUNAS_ID + 1)) -G1,2,$RUNAS_ID ls $DIR/d67 && error ||true
}
run_test 67 "supplementary group failure (should return error) ="

cleanup_68() {
       if [ "$LOOPDEV" ]; then
               swapoff $LOOPDEV || error "swapoff failed"
               losetup -d $LOOPDEV || error "losetup -d failed"
               unset LOOPDEV LOOPNUM
       fi
       rm -f $DIR/f68
}

meminfo() {
       awk '($1 == "'$1':") { print $2 }' /proc/meminfo
}

swap_used() {
       swapon -s | awk '($1 == "'$1'") { print $4 }'
}

# excercise swapping to lustre by adding a high priority swapfile entry
# and then consuming memory until it is used.
test_68() {
       [ "$UID" != 0 ] && echo "skipping test 68 (must run as root)" && return
       [ "`lsmod|grep obdfilter`" ] && echo "skipping test 68 (local OST)" && \
               return

       find_loop_dev
       dd if=/dev/zero of=$DIR/f68 bs=64k count=1024

       trap cleanup_68 EXIT

       losetup $LOOPDEV $DIR/f68 || error "losetup $LOOPDEV failed"
       mkswap $LOOPDEV
       swapon -p 32767 $LOOPDEV || error "swapon $LOOPDEV failed"

       echo "before: `swapon -s | grep $LOOPDEV`"
       KBFREE=`meminfo MemTotal`
       $MEMHOG $KBFREE || error "error allocating $KBFREE kB"
       echo "after: `swapon -s | grep $LOOPDEV`"
       SWAPUSED=`swap_used $LOOPDEV`

       cleanup_68

       [ $SWAPUSED -eq 0 ] && echo "no swap used???" || true
}
run_test 68 "support swapping to Lustre ========================"

# bug 3462 - multiple simultaneous MDC requests
test_69() {
       mkdir $DIR/D68-1 
       mkdir $DIR/D68-2
       multiop $DIR/D68-1/f68-1 O_c &
       pid1=$!
       #give multiop a chance to open
       usleep 500

       echo 0x80000129 > /proc/sys/lustre/fail_loc
       multiop $DIR/D68-1/f68-2 Oc &
       sleep 1
       echo 0 > /proc/sys/lustre/fail_loc

       multiop $DIR/D68-2/f68-3 Oc &
       pid3=$!

       kill -USR1 $pid1
       wait $pid1 || return 1

       sleep 25

       $CHECKSTAT -t file $DIR/D68-1/f68-1 || return 4
       $CHECKSTAT -t file $DIR/D68-1/f68-2 || return 5 
       $CHECKSTAT -t file $DIR/D68-2/f68-3 || return 6 

       rm -rf $DIR/D68-*
}
run_test 69 "multiple MDC requests (should not deadlock)"


test_70() {
	STAT="/proc/fs/lustre/osc/OSC*MNT*/stats"
	mkdir $DIR/d70
	dd if=/dev/zero of=$DIR/d70/file bs=512 count=5
	cancel_lru_locks OSC
	cat $DIR/d70/file >/dev/null
	# Hopefully there is only one.
	ENQ=`cat $STAT|awk -vnum=0 '/ldlm_enq/ {num += $2} END {print num;}'`
	CONV=`cat $STAT|awk -vnum=0 '/ldlm_conv/ {num += $2} END {print num;}'`
	CNCL=`cat $STAT|awk -vnum=0 '/ldlm_canc/ {num += $2} END {print num;}'`
	dd if=/dev/zero of=$DIR/d70/file bs=512 count=5
	ENQ1=`cat $STAT|awk -vnum=0 '/ldlm_enq/ {num += $2} END {print num;}'`
	CONV1=`cat $STAT|awk -vnum=0 '/ldlm_conv/ {num += $2} END {print num;}'`
	CNCL1=`cat $STAT|awk -vnum=0 '/ldlm_canc/ {num += $2} END {print num;}'`

	if [ $CONV1 -le $CONV ] ; then
		error "No conversion happened. Before: enq $ENQ, conv $CONV, cancel $CNCL ; After: enq $ENQ1, conv $CONV1, cancel $CNCL1"
	else
		echo "OK"
		true
	fi

}
run_test 70 "Test that PR->PW conversion takes place ==========="

test_71() {
	cp `which dbench` $DIR
	
	[ ! -f $DIR/dbench ] && echo "dbench not installed, skip this test" && return 0

	TGT=$DIR/client.txt
	SRC=${SRC:-/usr/lib/dbench/client.txt}
	[ ! -e $TGT -a -e $SRC ] && echo "copying $SRC to $TGT" && cp $SRC $TGT
	SRC=/usr/lib/dbench/client_plain.txt
	[ ! -e $TGT -a -e $SRC ] && echo "copying $SRC to $TGT" && cp $SRC $TGT

	echo "copying /lib to $DIR"
	cp -r /lib $DIR/lib
	
	echo "chroot $DIR /dbench -c client.txt 2"
	chroot $DIR /dbench -c client.txt 2
	RC=$?

	rm -f $DIR/dbench
	rm -f $TGT
	rm -fr $DIR/lib

	return $RC
}
run_test 71 "Running dbench on lustre (don't segment fault) ===="

# on the LLNL clusters, runas will still pick up root's $TMP settings,
# which will not be writable for the runas user, and then you get a CVS
# error message with a corrupt path string (CVS bug) and panic.
# We're not using much space, so just stick it in /tmp, which is safe.
OLDTMPDIR=$TMPDIR
OLDTMP=$TMP
TMPDIR=/tmp
TMP=/tmp
OLDHOME=$HOME
[ $RUNAS_ID -ne $UID ] && HOME=/tmp

test_99a() {
	mkdir -p $DIR/d99cvsroot
	chown $RUNAS_ID $DIR/d99cvsroot
	$RUNAS cvs -d $DIR/d99cvsroot init || error
}
run_test 99a "cvs init ========================================="

test_99b() {
	[ ! -d $DIR/d99cvsroot ] && test_99a
	cd /etc/init.d
	# some versions of cvs import exit(1) when asked to import links or
	# files they can't read.  ignore those files.
	TOIGNORE=$(find . -type l -printf '-I %f\n' -o \
			! -perm +4 -printf '-I %f\n')
	$RUNAS cvs -d $DIR/d99cvsroot import -m "nomesg" $TOIGNORE \
		d99reposname vtag rtag
}
run_test 99b "cvs import ======================================="

test_99c() {
	[ ! -d $DIR/d99cvsroot ] && test_99b
	cd $DIR
	mkdir -p $DIR/d99reposname
	chown $RUNAS_ID $DIR/d99reposname
	$RUNAS cvs -d $DIR/d99cvsroot co d99reposname
}
run_test 99c "cvs checkout ====================================="

test_99d() {
	[ ! -d $DIR/d99cvsroot ] && test_99c
	cd $DIR/d99reposname
	$RUNAS touch foo99
	$RUNAS cvs add -m 'addmsg' foo99
}
run_test 99d "cvs add =========================================="

test_99e() {
	[ ! -d $DIR/d99cvsroot ] && test_99c
	cd $DIR/d99reposname
	$RUNAS cvs update
}
run_test 99e "cvs update ======================================="

test_99f() {
	[ ! -d $DIR/d99cvsroot ] && test_99d
	cd $DIR/d99reposname
	$RUNAS cvs commit -m 'nomsg' foo99
}
run_test 99f "cvs commit ======================================="

test_100() {
        netstat -tan | while read PROT SND RCV LOCAL REMOTE STAT; do
                LPORT=`echo $LOCAL | cut -d: -f2`
                RPORT=`echo $REMOTE | cut -d: -f2`
                if [ "$PROT" = "tcp" ] && [ "$LPORT" != "*" ] && [ "$RPORT" != "*" ] && [ $RPORT -eq 988 ] && [ $LPORT -gt 1024 ]; then
                        echo "local port: $LPORT > 1024"
                        error
                fi
        done
}
run_test 100 "check local port using privileged port ==========="


TMPDIR=$OLDTMPDIR
TMP=$OLDTMP
HOME=$OLDHOME

log "cleanup: ======================================================"
if [ "`mount | grep ^$NAME`" ]; then
	rm -rf $DIR/[Rdfs][1-9]*
	if [ "$I_MOUNTED" = "yes" ]; then
		sh llmountcleanup.sh || error
	fi
fi

echo '=========================== finished ==============================='
[ -f "$SANITYLOG" ] && cat $SANITYLOG && exit 1 || true
