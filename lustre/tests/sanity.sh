#!/bin/bash
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# e.g. ONLY="22 23" or ONLY="`seq 32 39`" or EXCEPT="31"
set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped test: 4900 4900 2108 9789 3637 9789 3561 12622 13310 10764
ALWAYS_EXCEPT=${ALWAYS_EXCEPT:-"27o  27q  42a  42b  42c  42d  45  51d   74b   75 $SANITY_EXCEPT" }
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

[ "$SLOW" = "no" ] && EXCEPT_SLOW="24o 27m 36f 36g 51b 51c 60c 63 64b 68 71 73 78 101 103 115 120g"

# Tests that fail on uml, maybe elsewhere, FIXME
CPU=`awk '/model/ {print $4}' /proc/cpuinfo`
#                                    buffer i/o errs             sock spc runas
[ "$CPU" = "UML" ] && EXCEPT="$EXCEPT 27m 27n 27o 27p 27q 27r 31d 54a  64b 99a 99b 99c 99d 99e 99f 101"

case `uname -r` in
2.4*) FSTYPE=${FSTYPE:-ext3};    ALWAYS_EXCEPT="$ALWAYS_EXCEPT 76"
	[ "$CPU" = "UML" ] && ALWAYS_EXCEPT="$ALWAYS_EXCEPT 105a";;
2.6*) FSTYPE=${FSTYPE:-ldiskfs}; ALWAYS_EXCEPT="$ALWAYS_EXCEPT " ;;
*) error "unsupported kernel" ;;
esac

SRCDIR=`dirname $0`
export PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH:/sbin

TMP=${TMP:-/tmp}

CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFS=${LFS:-lfs}
SETSTRIPE=${SETSTRIPE:-"$LFS setstripe"}
GETSTRIPE=${GETSTRIPE:-"$LFS getstripe"}
LSTRIPE=${LSTRIPE:-"$LFS setstripe"}
LFIND=${LFIND:-"$LFS find"}
LVERIFY=${LVERIFY:-ll_dirstripe_verify}
LSTRIPEINFO=${LSTRIPEINFO:-ll_getstripe_info}
LCTL=${LCTL:-lctl}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
RANDOM_READS=${RANDOM_READS:-"random-reads"}
TOEXCL=${TOEXCL:-toexcl}
TRUNCATE=${TRUNCATE:-truncate}
MUNLINK=${MUNLINK:-munlink}
SOCKETSERVER=${SOCKETSERVER:-socketserver}
SOCKETCLIENT=${SOCKETCLIENT:-socketclient}
IOPENTEST1=${IOPENTEST1:-iopentest1}
IOPENTEST2=${IOPENTEST2:-iopentest2}
MEMHOG=${MEMHOG:-memhog}
DIRECTIO=${DIRECTIO:-directio}
ACCEPTOR_PORT=${ACCEPTOR_PORT:-988}
UMOUNT=${UMOUNT:-"umount -d"}
CHECK_GRANT=${CHECK_GRANT:-"yes"}
GRANT_CHECK_LIST=${GRANT_CHECK_LIST:-""}

export NAME=${NAME:-local}

SAVE_PWD=$PWD

CLEANUP=${CLEANUP:-:}
SETUP=${SETUP:-:}
TRACE=${TRACE:-""}
LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

SANITYLOG=${TESTSUITELOG:-$TMP/$(basename $0 .sh).log}
FAIL_ON_ERROR=false

cleanup() {
	echo -n "cln.."
	pgrep ll_sa > /dev/null && { echo "There are ll_sa thread not exit!"; exit 20; }
	cleanupall ${FORCE} $* || { echo "FAILed to clean up"; exit 20; }
}
setup() {
	echo -n "mnt.."
        load_modules
	setupall || exit 10
	echo "done"
}


check_kernel_version() {
	VERSION_FILE=$LPROC/version
	WANT_VER=$1
	[ ! -f $VERSION_FILE ] && echo "can't find kernel version" && return 1
	GOT_VER=$(awk '/kernel:/ {print $2}' $VERSION_FILE)
	[ $GOT_VER == "patchless" ] && return 0
	[ $GOT_VER -ge $WANT_VER ] && return 0
	log "test needs at least kernel version $WANT_VER, running $GOT_VER"
	return 1
}

if [ "$ONLY" == "cleanup" ]; then
 	sh llmountcleanup.sh
 	exit 0
fi

[ "$SANITYLOG" ] && rm -f $SANITYLOG || true

check_and_setup_lustre

DIR=${DIR:-$MOUNT}
[ -z "`echo $DIR | grep $MOUNT`" ] && echo "$DIR not in $MOUNT" && exit 99

LOVNAME=`cat $LPROC/llite/*/lov/common_name | tail -n 1`
OSTCOUNT=`cat $LPROC/lov/$LOVNAME/numobd`
STRIPECOUNT=`cat $LPROC/lov/$LOVNAME/stripecount`
STRIPESIZE=`cat $LPROC/lov/$LOVNAME/stripesize`
ORIGFREE=`cat $LPROC/lov/$LOVNAME/kbytesavail`
MAXFREE=${MAXFREE:-$((200000 * $OSTCOUNT))}

[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
[ -f $DIR/d52b/foo ] && chattr -i $DIR/d52b/foo
rm -rf $DIR/[Rdfs][1-9]*

if [ $UID -ne 0 ]; then
	log "running as non-root uid $UID"
	RUNAS_ID="$UID"
	RUNAS=""
else
	RUNAS_ID=${RUNAS_ID:-500}
	RUNAS=${RUNAS:-"runas -u $RUNAS_ID"}

	# $RUNAS_ID may get set incorrectly somewhere else
	[ $RUNAS_ID -eq 0 ] && error "\$RUNAS_ID set to 0, but \$UID is also 0!"
fi

check_runas_id $RUNAS_ID $RUNAS

build_test_filter

echo "preparing for tests involving mounts"
EXT2_DEV=${EXT2_DEV:-$TMP/SANITY.LOOP}
touch $EXT2_DEV
mke2fs -j -F $EXT2_DEV 8000 > /dev/null
echo # add a newline after mke2fs.

umask 077

OLDDEBUG="`sysctl lnet.debug 2> /dev/null`"
sysctl -w lnet.debug=-1 2> /dev/null || true
test_0() {
	touch $DIR/$tfile
	$CHECKSTAT -t file $DIR/$tfile || error
	rm $DIR/$tfile
	$CHECKSTAT -a $DIR/$tfile || error
}
run_test 0 "touch .../$tfile ; rm .../$tfile ====================="

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
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	if [ ! -f $DIR/f6a ]; then
		touch $DIR/f6a
		chmod 0666 $DIR/f6a
	fi
	$RUNAS chmod 0444 $DIR/f6a && error
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/f6a || error
}
run_test 6b "$RUNAS chmod .../f6a (should return error) =="

test_6c() {
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	touch $DIR/f6c
	chown $RUNAS_ID $DIR/f6c || error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/f6c || error
}
run_test 6c "touch .../f6c; chown .../f6c ======================"

test_6d() {
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	if [ ! -f $DIR/f6c ]; then
		touch $DIR/f6c
		chown $RUNAS_ID $DIR/f6c
	fi
	$RUNAS chown $UID $DIR/f6c && error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/f6c || error
}
run_test 6d "$RUNAS chown .../f6c (should return error) =="

test_6e() {
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	touch $DIR/f6e
	chgrp $RUNAS_ID $DIR/f6e || error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/f6e || error
}
run_test 6e "touch .../f6e; chgrp .../f6e ======================"

test_6f() {
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	if [ ! -f $DIR/f6e ]; then
		touch $DIR/f6e
		chgrp $RUNAS_ID $DIR/f6e
	fi
	$RUNAS chgrp $UID $DIR/f6e && error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/f6e || error
}
run_test 6f "$RUNAS chgrp .../f6e (should return error) =="

test_6g() {
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
        mkdir $DIR/d6g || error
        chmod 777 $DIR/d6g || error
        $RUNAS mkdir $DIR/d6g/d || error
        chmod g+s $DIR/d6g/d || error
        mkdir $DIR/d6g/d/subdir
	$CHECKSTAT -g \#$RUNAS_ID $DIR/d6g/d/subdir || error
}
run_test 6g "Is new dir in sgid dir inheriting group?"

test_6h() { # bug 7331
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	touch $DIR/f6h || error "touch failed"
	chown $RUNAS_ID:$RUNAS_ID $DIR/f6h || error "initial chown failed"
	$RUNAS -G$RUNAS_ID chown $RUNAS_ID:0 $DIR/f6h && error "chown worked"
	$CHECKSTAT -t file -u \#$RUNAS_ID -g \#$RUNAS_ID $DIR/f6h || error
}
run_test 6h "$RUNAS chown RUNAS_ID.0 .../f6h (should return error)"

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
	rm -f $DIR/d17/l-exist
	$CHECKSTAT -a $DIR/d17/l-exist || error
}
run_test 17a "symlinks: create, remove (real) =================="

test_17b() {
	mkdir -p $DIR/d17
	ln -s no-such-file $DIR/d17/l-dangle
	ls -l $DIR/d17
	$CHECKSTAT -l no-such-file $DIR/d17/l-dangle || error
	$CHECKSTAT -fa $DIR/d17/l-dangle || error
	rm -f $DIR/d17/l-dangle
	$CHECKSTAT -a $DIR/d17/l-dangle || error
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

test_17e() {
	mkdir -p $DIR/$tdir
	local foo=$DIR/$tdir/$tfile
	ln -s $foo $foo || error "create symlink failed"
	ls -l $foo || error "ls -l failed"
	ls $foo && error "ls not failed" || true
}
run_test 17e "symlinks: create recursive symlink (should return error) ===="

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
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
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
	WDIR=$DIR/$tdir
	chown $RUNAS_ID $WDIR
	(cd $WDIR || error "cd $WDIR failed";
	$RUNAS tar cf - /etc/hosts /etc/sysconfig/network | \
	$RUNAS tar xf -)
	ls -lR $WDIR/etc || error "ls -lR $WDIR/etc failed"
	$CHECKSTAT -t dir $WDIR/etc || error "checkstat -t dir failed"
	$CHECKSTAT -u \#$RUNAS_ID $WDIR/etc || error "checkstat -u failed"
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
	multiop $f OcLN ${f}2 ${f}2 || error "link ${f}2 ${f}2 failed"
	# on ext3 this does not remove either the source or target files
	# though the "expected" operation would be to remove the source
	$CHECKSTAT -t file ${f} || error "${f} missing"
	$CHECKSTAT -t file ${f}2 || error "${f}2 missing"
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
run_test 24n "Statting the old file after renaming (Posix rename 2)"

test_24o() {
	check_kernel_version 37 || return 0
	mkdir -p $DIR/d24o
	rename_many -s random -v -n 10 $DIR/d24o
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
	usleep 500

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

test_24u() { # bug12192
        multiop $DIR/$tfile C2w$((2048 * 1024))c || error
        $CHECKSTAT -s $((2048 * 1024)) $DIR/$tfile || error "wrong file size"
}
run_test 24u "create stripe file"

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

# recursive symlinks (bug 7022)
test_26f() {
	mkdir $DIR/$tdir/$tfile        || error "mkdir $DIR/$tdir/$tfile failed"
	cd $DIR/$tdir/$tfile           || error "cd $DIR/$tdir/$tfile failed"
	mkdir -p lndir/bar1      || error "mkdir lndir/bar1 failed"
	mkdir $tfile             || error "mkdir $tfile failed"
	cd $tfile                || error "cd $tfile failed"
	ln -s .. dotdot          || error "ln dotdot failed"
	ln -s dotdot/lndir lndir || error "ln lndir failed"
	cd $DIR/$tdir                 || error "cd $DIR/$tdir failed"
	output=`ls $tfile/$tfile/lndir/bar1`
	[ "$output" = bar1 ] && error "unexpected output"
	rm -r $tfile             || error "rm $tfile failed"
	$CHECKSTAT -a $DIR/$tfile || error "$tfile not gone"
}
run_test 26f "rm -r of a directory which has recursive symlink ="

test_27a() {
	echo '== stripe sanity =============================================='
	mkdir -p $DIR/d27 || error "mkdir failed"
	$SETSTRIPE $DIR/d27/f0 -c 1 || error "lstripe failed"
	$CHECKSTAT -t file $DIR/d27/f0 || error "checkstat failed"
	pass
	log "== test 27b: write to one stripe file ========================="
	cp /etc/hosts $DIR/d27/f0 || error
}
run_test 27a "one stripe file =================================="

test_27c() {
	[ "$OSTCOUNT" -lt "2" ] && skip "skipping 2-stripe test" && return
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/f01 -c 2 || error "lstripe failed"
	[ `$GETSTRIPE $DIR/d27/f01 | grep -A 10 obdidx | wc -l` -eq 4 ] ||
		error "two-stripe file doesn't have two stripes"
	pass
	log "== test 27d: write to two stripe file file f01 ================"
	dd if=/dev/zero of=$DIR/d27/f01 bs=4k count=4 || error "dd failed"
}
run_test 27c "create two stripe file f01 ======================="

test_27d() {
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/fdef 0 -1 0 || error "lstripe failed"
	$CHECKSTAT -t file $DIR/d27/fdef || error "checkstat failed"
	dd if=/dev/zero of=$DIR/d27/fdef bs=4k count=4 || error
}
run_test 27d "create file with default settings ================"

test_27e() {
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/f12 -c 2 || error "lstripe failed"
	$SETSTRIPE $DIR/d27/f12 -c 2 && error "lstripe succeeded twice"
	$CHECKSTAT -t file $DIR/d27/f12 || error "checkstat failed"
}
run_test 27e "lstripe existing file (should return error) ======"

test_27f() {
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/fbad -s 100 -i 0 -c 1 && error "lstripe failed"
	dd if=/dev/zero of=$DIR/d27/f12 bs=4k count=4 || error "dd failed"
	$GETSTRIPE $DIR/d27/fbad || error "lfs getstripe failed"
}
run_test 27f "lstripe with bad stripe size (should return error)"

test_27g() {
	mkdir -p $DIR/d27
	$MCREATE $DIR/d27/fnone || error "mcreate failed"
	pass
	log "== test 27h: lfs getstripe with no objects ===================="
	$GETSTRIPE $DIR/d27/fnone 2>&1 | grep "no stripe info" || error "has object"
	pass
	log "== test 27i: lfs getstripe with some objects =================="
	touch $DIR/d27/fsome || error "touch failed"
	$GETSTRIPE $DIR/d27/fsome | grep obdidx || error "missing objects"
}
run_test 27g "test lfs getstripe ==========================================="

test_27j() {
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/f27j -i $OSTCOUNT && error "lstripe failed"||true
}
run_test 27j "lstripe with bad stripe offset (should return error)"

test_27k() { # bug 2844
	mkdir -p $DIR/d27
	FILE=$DIR/d27/f27k
	LL_MAX_BLKSIZE=$((4 * 1024 * 1024))
	[ ! -d $DIR/d27 ] && mkdir -p $DIR/d27
	$SETSTRIPE $FILE -s 67108864 || error "lstripe failed"
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
	dd if=/dev/zero of=$FILE bs=4k count=1
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
}
run_test 27k "limit i_blksize for broken user apps ============="

test_27l() {
	mkdir -p $DIR/d27
	mcreate $DIR/f27l || error "creating file"
	$RUNAS $SETSTRIPE $DIR/f27l -c 1 && \
		error "lstripe should have failed" || true
}
run_test 27l "check setstripe permissions (should return error)"

test_27m() {
	[ "$OSTCOUNT" -lt "2" ] && skip "$OSTCOUNT < 2 OSTs -- skipping" && return
	if [ $ORIGFREE -gt $MAXFREE ]; then
		skip "$ORIGFREE > $MAXFREE skipping out-of-space test on OST0"
		return
	fi
	mkdir -p $DIR/d27
	$SETSTRIPE $DIR/d27/f27m_1 -i 0 -c 1
	dd if=/dev/zero of=$DIR/d27/f27m_1 bs=1024 count=$MAXFREE && \
		error "dd should fill OST0"
	i=2
	while $SETSTRIPE $DIR/d27/f27m_$i -i 0 -c 1 ; do
		i=`expr $i + 1`
		[ $i -gt 256 ] && break
	done
	i=`expr $i + 1`
	touch $DIR/d27/f27m_$i
	[ `$GETSTRIPE $DIR/d27/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] && \
		error "OST0 was full but new created file still use it"
	i=`expr $i + 1`
	touch $DIR/d27/f27m_$i
	[ `$GETSTRIPE $DIR/d27/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] && \
		error "OST0 was full but new created file still use it"
	rm -r $DIR/d27
}
run_test 27m "create file while OST0 was full =================="

# osc's keep a NOSPC stick flag that gets unset with rmdir
reset_enospc() {
	[ "$1" ] && FAIL_LOC=$1 || FAIL_LOC=0
	mkdir -p $DIR/d27/nospc
	rmdir $DIR/d27/nospc
	sysctl -w lustre.fail_loc=$FAIL_LOC
}

exhaust_precreations() {
	OSTIDX=$1
	OST=$(grep ${OSTIDX}": " $LPROC/lov/${LOVNAME}/target_obd | \
	    awk '{print $2}' | sed -e 's/_UUID$//')
	# on the mdt's osc
	last_id=$(cat $LPROC/osc/${OST}-osc/prealloc_last_id)
	next_id=$(cat $LPROC/osc/${OST}-osc/prealloc_next_id)

	mkdir -p $DIR/d27/${OST}
	$SETSTRIPE $DIR/d27/${OST} -i $OSTIDX -c 1
	#define OBD_FAIL_OST_ENOSPC 0x215
	sysctl -w lustre.fail_loc=0x215
	echo "Creating to objid $last_id on ost $OST..."
	createmany -o $DIR/d27/${OST}/f $next_id $((last_id - next_id + 2))
	grep '[0-9]' $LPROC/osc/${OST}-osc/prealloc*
	reset_enospc $2
}

exhaust_all_precreations() {
	local i
	for (( i=0; i < OSTCOUNT; i++ )) ; do
		exhaust_precreations $i 0x215
	done
	reset_enospc $1
}

test_27n() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	reset_enospc
	rm -f $DIR/d27/f27n
	exhaust_precreations 0 0x80000215

	touch $DIR/d27/f27n || error

	reset_enospc
}
run_test 27n "create file with some full OSTs =================="

test_27o() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	reset_enospc
	rm -f $DIR/d27/f27o
	exhaust_all_precreations 0x215
	sleep 5

	touch $DIR/d27/f27o && error "able to create $DIR/d27/f27o"

	reset_enospc
}
run_test 27o "create file with all full OSTs (should error) ===="

test_27p() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	reset_enospc
	rm -f $DIR/d27/f27p

	$MCREATE $DIR/d27/f27p || error
	$TRUNCATE $DIR/d27/f27p 80000000 || error
	$CHECKSTAT -s 80000000 $DIR/d27/f27p || error

	exhaust_precreations 0 0x80000215
	echo foo >> $DIR/d27/f27p || error
	$CHECKSTAT -s 80000004 $DIR/d27/f27p || error

	reset_enospc
}
run_test 27p "append to a truncated file with some full OSTs ==="

test_27q() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	reset_enospc
	rm -f $DIR/d27/f27q

	$MCREATE $DIR/d27/f27q || error "mcreate $DIR/d27/f27q failed"
	$TRUNCATE $DIR/d27/f27q 80000000 ||error "truncate $DIR/d27/f27q failed"
	$CHECKSTAT -s 80000000 $DIR/d27/f27q || error "checkstat failed"

	exhaust_all_precreations 0x215

	echo foo >> $DIR/d27/f27q && error "append succeeded"
	$CHECKSTAT -s 80000000 $DIR/d27/f27q || error "checkstat 2 failed"

	reset_enospc
}
run_test 27q "append to truncated file with all OSTs full (should error) ==="

test_27r() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	reset_enospc
	rm -f $DIR/d27/f27r
	exhaust_precreations 0 0x80000215

	$SETSTRIPE $DIR/d27/f27r -i 0 -c 2 # && error

	reset_enospc
}
run_test 27r "stripe file with some full OSTs (shouldn't LBUG) ="

test_27s() { # bug 10725
	mkdir -p $DIR/$tdir
	$LSTRIPE $DIR/$tdir $((2048 * 1024 * 1024)) -1 2 && \
		error "stripe width >= 2^32 succeeded" || true
}
run_test 27s "lsm_xfersize overflow (should error) (bug 10725)"

test_27t() { # bug 10864
        WDIR=`pwd`
        WLFS=`which lfs`
        cd $DIR
        touch $tfile
        $WLFS getstripe $tfile
        cd $WDIR
}
run_test 27t "check that utils parse path correctly"

test_27u() { # bug 4900
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

        #define OBD_FAIL_MDS_OSC_PRECREATE      0x139

        sysctl -w lustre.fail_loc=0x139
        mkdir -p $DIR/d27u
        createmany -o $DIR/d27u/t- 1000
        sysctl -w lustre.fail_loc=0

        TLOG=$DIR/$tfile.getstripe
        $GETSTRIPE $DIR/d27u > $TLOG
        OBJS=`awk -vobj=0 '($1 == 0) { obj += 1 } END { print obj;}' $TLOG`
        unlinkmany $DIR/d27u/t- 1000
        [ $OBJS -gt 0 ] && \
                error "$OBJS objects created on OST-0.  See $TLOG" || pass
}
run_test 27u "skip object creation on OSC w/o objects =========="

test_27v() { # bug 4900
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

        exhaust_all_precreations

        mkdir -p $DIR/$tdir
        $SETSTRIPE $DIR/$tdir -c 1         # 1 stripe / file

        touch $DIR/$tdir/$tfile
        #define OBD_FAIL_TGT_DELAY_PRECREATE     0x705
        sysctl -w lustre.fail_loc=0x705
        START=`date +%s`
        for F in `seq 1 32`; do
                touch $DIR/$tdir/$tfile.$F
        done
        sysctl -w lustre.fail_loc=0

        FINISH=`date +%s`
        TIMEOUT=`sysctl -n lustre.timeout`
        [ $((FINISH - START)) -ge $((TIMEOUT / 2)) ] && \
               error "$FINISH - $START >= $TIMEOUT / 2"

        reset_enospc
}
run_test 27v "skip object creation on slow OST ================="

test_27w() { # bug 10997
        mkdir -p $DIR/d27w || error "mkdir failed"
        $LSTRIPE $DIR/d27w/f0 -s 65536 || error "lstripe failed"
        size=`$LSTRIPEINFO $DIR/d27w/f0 | awk {'print $1'}`
        [ $size -ne 65536 ] && error "stripe size $size != 65536" || true

        [ "$OSTCOUNT" -lt "2" ] && skip "skipping multiple stripe count/offset test" && return
        for i in `seq 1 $OSTCOUNT`; do
                offset=$(($i-1))
                $LSTRIPE $DIR/d27w/f$i -c $i -i $offset || error "lstripe -c $i -i $offset failed"
                count=`$LSTRIPEINFO $DIR/d27w/f$i | awk {'print $2'}`
                index=`$LSTRIPEINFO $DIR/d27w/f$i | awk {'print $3'}`
                [ $count -ne $i ] && error "stripe count $count != $i" || true
                [ $index -ne $offset ] && error "stripe offset $index != $offset" || true
        done
}
run_test 27w "check lfs setstripe -c -s -i options ============="

test_28() {
	mkdir $DIR/d28
	$CREATETEST $DIR/d28/ct || error
}
run_test 28 "create/mknod/mkdir with bad file types ============"

test_29() {
	cancel_lru_locks mdc
	mkdir $DIR/d29
	touch $DIR/d29/foo
	log 'first d29'
	ls -l $DIR/d29
	MDCDIR=${MDCDIR:-`find $LPROC/ldlm/namespaces | grep mdc | head -1`}
	LOCKCOUNTORIG=`cat $MDCDIR/lock_count`
	LOCKUNUSEDCOUNTORIG=`cat $MDCDIR/lock_unused_count`
	[ -z $"LOCKCOUNTORIG" ] && echo "No mdc lock count" && return 1
	log 'second d29'
	ls -l $DIR/d29
	log 'done'
	LOCKCOUNTCURRENT=`cat $MDCDIR/lock_count`
	LOCKUNUSEDCOUNTCURRENT=`cat $MDCDIR/lock_unused_count`
	if [ "$LOCKCOUNTCURRENT" -gt "$LOCKCOUNTORIG" ]; then
		echo > $LPROC/ldlm/dump_namespaces
		error "CURRENT: $LOCKCOUNTCURRENT > $LOCKCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 2
	fi
	if [ "$LOCKUNUSEDCOUNTCURRENT" -gt "$LOCKUNUSEDCOUNTORIG" ]; then
		error "UNUSED: $LOCKUNUSEDCOUNTCURRENT > $LOCKUNUSEDCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 3
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
	$SETSTRIPE $DIR/d31f -s 1048576 -c 1
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	$GETSTRIPE $DIR/d31f/hosts
	multiop $DIR/d31f D_c &
	MULTIPID=$!

	sleep 1

	rm -rv $DIR/d31f || error "first of $DIR/d31f"
	mkdir $DIR/d31f
	$SETSTRIPE $DIR/d31f -s 1048576 -c 1
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	 $DIR/d31f/hosts
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
	$UMOUNT $DIR/d32a/ext2-mountpoint || error
}
run_test 32a "stat d32a/ext2-mountpoint/.. ====================="

test_32b() {
	[ -e $DIR/d32b ] && rm -fr $DIR/d32b
	mkdir -p $DIR/d32b/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32b/ext2-mountpoint || error
	ls -al $DIR/d32b/ext2-mountpoint/.. || error
	$UMOUNT $DIR/d32b/ext2-mountpoint || error
}
run_test 32b "open d32b/ext2-mountpoint/.. ====================="
 
test_32c() {
	[ -e $DIR/d32c ] && rm -fr $DIR/d32c
	mkdir -p $DIR/d32c/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32c/ext2-mountpoint || error
	mkdir -p $DIR/d32c/d2/test_dir    
	$CHECKSTAT -t dir $DIR/d32c/ext2-mountpoint/../d2/test_dir || error
	$UMOUNT $DIR/d32c/ext2-mountpoint || error
}
run_test 32c "stat d32c/ext2-mountpoint/../d2/test_dir ========="

test_32d() {
	[ -e $DIR/d32d ] && rm -fr $DIR/d32d
	mkdir -p $DIR/d32d/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32d/ext2-mountpoint || error
	mkdir -p $DIR/d32d/d2/test_dir    
	ls -al $DIR/d32d/ext2-mountpoint/../d2/test_dir || error
	$UMOUNT $DIR/d32d/ext2-mountpoint || error
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
	TMP_DIR=$DIR/$tdir/tmp       
	mkdir -p $TMP_DIR $DIR/${tdir}2
	ln -s $DIR/${tdir}2 $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	$CHECKSTAT -t link $TMP_DIR/symlink12 || error
	$CHECKSTAT -t link $DIR/$tdir/symlink02 || error
	$CHECKSTAT -t dir -f $TMP_DIR/symlink12 || error
	$CHECKSTAT -t dir -f $DIR/$tdir/symlink02 || error
}
run_test 32g "stat d32g/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32h() {
	rm -fr $DIR/$tdir $DIR/${tdir}2
	TMP_DIR=$DIR/$tdir/tmp       
	mkdir -p $TMP_DIR $DIR/${tdir}2 
	ln -s $DIR/${tdir}2 $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	ls $TMP_DIR/symlink12 || error
	ls $DIR/$tdir/symlink02  || error
}
run_test 32h "open d32h/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32i() {
	[ -e $DIR/d32i ] && rm -fr $DIR/d32i
	mkdir -p $DIR/d32i/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32i/ext2-mountpoint || error
	touch $DIR/d32i/test_file
	$CHECKSTAT -t file $DIR/d32i/ext2-mountpoint/../test_file || error  
	$UMOUNT $DIR/d32i/ext2-mountpoint || error
}
run_test 32i "stat d32i/ext2-mountpoint/../test_file ==========="

test_32j() {
	[ -e $DIR/d32j ] && rm -fr $DIR/d32j
	mkdir -p $DIR/d32j/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32j/ext2-mountpoint || error
	touch $DIR/d32j/test_file
	cat $DIR/d32j/ext2-mountpoint/../test_file || error
	$UMOUNT $DIR/d32j/ext2-mountpoint || error
}
run_test 32j "open d32j/ext2-mountpoint/../test_file ==========="

test_32k() {
	rm -fr $DIR/d32k
	mkdir -p $DIR/d32k/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32k/ext2-mountpoint  
	mkdir -p $DIR/d32k/d2
	touch $DIR/d32k/d2/test_file || error
	$CHECKSTAT -t file $DIR/d32k/ext2-mountpoint/../d2/test_file || error
	$UMOUNT $DIR/d32k/ext2-mountpoint || error
}
run_test 32k "stat d32k/ext2-mountpoint/../d2/test_file ========"

test_32l() {
	rm -fr $DIR/d32l
	mkdir -p $DIR/d32l/ext2-mountpoint 
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32l/ext2-mountpoint || error
	mkdir -p $DIR/d32l/d2
	touch $DIR/d32l/d2/test_file
	cat  $DIR/d32l/ext2-mountpoint/../d2/test_file || error
	$UMOUNT $DIR/d32l/ext2-mountpoint || error
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
	rm -fr $DIR/d32o $DIR/$tfile
	touch $DIR/$tfile 
	mkdir -p $DIR/d32o/tmp    
	TMP_DIR=$DIR/d32o/tmp       
	ln -s $DIR/$tfile $TMP_DIR/symlink12 
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
	$CHECKSTAT -t link $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t link $DIR/d32o/symlink02 || error
	$CHECKSTAT -t file -f $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t file -f $DIR/d32o/symlink02 || error
}
run_test 32o "stat d32o/symlink->tmp/symlink->lustre-root/$tfile"

test_32p() {
    log 32p_1
	rm -fr $DIR/d32p
    log 32p_2
	rm -f $DIR/$tfile
    log 32p_3
	touch $DIR/$tfile 
    log 32p_4
	mkdir -p $DIR/d32p/tmp    
    log 32p_5
	TMP_DIR=$DIR/d32p/tmp       
    log 32p_6
	ln -s $DIR/$tfile $TMP_DIR/symlink12 
    log 32p_7
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02 
    log 32p_8
	cat $DIR/d32p/tmp/symlink12 || error
    log 32p_9
	cat $DIR/d32p/symlink02 || error
    log 32p_10
}
run_test 32p "open d32p/symlink->tmp/symlink->lustre-root/$tfile"

test_32q() {
	[ -e $DIR/d32q ] && rm -fr $DIR/d32q
	mkdir -p $DIR/d32q
        touch $DIR/d32q/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32q
	ls $DIR/d32q/under_the_mount && error || true
	$UMOUNT $DIR/d32q || error
}
run_test 32q "stat follows mountpoints in Lustre (should return error)"

test_32r() {
	[ -e $DIR/d32r ] && rm -fr $DIR/d32r
	mkdir -p $DIR/d32r
        touch $DIR/d32r/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/d32r
	ls $DIR/d32r | grep -q under_the_mount && error || true
	$UMOUNT $DIR/d32r || error
}
run_test 32r "opendir follows mountpoints in Lustre (should return error)"

test_33() {
	rm -f $DIR/$tfile
	touch $DIR/$tfile
	chmod 444 $DIR/$tfile
	chown $RUNAS_ID $DIR/$tfile
	log 33_1
	$RUNAS $OPENFILE -f O_RDWR $DIR/$tfile && error || true
	log 33_2
}
run_test 33 "write file with mode 444 (should return error) ===="

test_33a() {
        rm -fr $DIR/d33
        mkdir -p $DIR/d33
        chown $RUNAS_ID $DIR/d33
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33|| error "create"
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33 && \
		error "open RDWR" || true
}
run_test 33a "test open file(mode=0444) with O_RDWR (should return error)"

test_33b() {
        rm -fr $DIR/d33
        mkdir -p $DIR/d33
        chown $RUNAS_ID $DIR/d33
        $RUNAS $OPENFILE -f 1286739555 $DIR/d33/f33 && error "create" || true
}
run_test 33b "test open file with malformed flags (No panic and return error)"

TEST_34_SIZE=${TEST_34_SIZE:-2000000000000}
test_34a() {
	rm -f $DIR/f34
	$MCREATE $DIR/f34 || error
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$TRUNCATE $DIR/f34 $TEST_34_SIZE || error
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34a "truncate file that has not been opened ==========="

test_34b() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDONLY $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34b "O_RDONLY opening file doesn't create objects ====="

test_34c() {
	[ ! -f $DIR/f34 ] && test_34a 
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDWR $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" && error
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

test_34f() { # bug 6242, 6243
	SIZE34F=48000
	rm -f $DIR/f34f
	$MCREATE $DIR/f34f || error
	$TRUNCATE $DIR/f34f $SIZE34F || error "truncating $DIR/f3f to $SIZE34F"
	dd if=$DIR/f34f of=$TMP/f34f
	$CHECKSTAT -s $SIZE34F $TMP/f34f || error "$TMP/f34f not $SIZE34F bytes"
	dd if=/dev/zero of=$TMP/f34fzero bs=$SIZE34F count=1
	cmp $DIR/f34f $TMP/f34fzero || error "$DIR/f34f not all zero"
	cmp $TMP/f34f $TMP/f34fzero || error "$TMP/f34f not all zero"
	rm $TMP/f34f $TMP/f34fzero $DIR/f34f
}
run_test 34f "read from a file with no objects until EOF ======="

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
	[ $RUNAS_ID -eq $UID ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	mkdir -p $DIR/$tdir
	touch $DIR/$tdir/$tfile
	$RUNAS utime $DIR/$tdir/$tfile && \
		error "utime worked, expected failure" || true
}
run_test 36e "utime on non-owned file (should return error) ===="

test_36f() {
	export LANG=C LC_LANG=C # for date language

	DATESTR="Dec 20  2000"
	mkdir -p $DIR/$tdir
	#define OBD_FAIL_OST_BRW_PAUSE_BULK 0x214
        sysctl -w lustre.fail_loc=0x80000214
	date; date +%s
	cp /etc/hosts $DIR/$tdir/$tfile
	sync & # write RPC generated with "current" inode timestamp, but delayed
	sleep 1
	touch --date="$DATESTR" $DIR/$tdir/$tfile # setattr timestamp in past
	LS_BEFORE="`ls -l $DIR/$tdir/$tfile`" # old timestamp from client cache
	cancel_lru_locks osc
	LS_AFTER="`ls -l $DIR/$tdir/$tfile`"  # timestamp from OST object
	date; date +%s
	[ "$LS_BEFORE" != "$LS_AFTER" ] && \
		echo "BEFORE: $LS_BEFORE" && \
		echo "AFTER : $LS_AFTER" && \
		echo "WANT  : $DATESTR" && \
		error "$DIR/$tdir/$tfile timestamps changed" || true
}
run_test 36f "utime on file racing with OST BRW write =========="

export FMD_MAX_AGE=`do_facet ost1 cat $LPROC/obdfilter/*/client_cache_seconds 2> /dev/null | head -n 1`
test_36g() {
	[ -z "$FMD_MAX_AGE" ] && skip "skip test for remote OST" && return
	FMD_BEFORE="`awk '/ll_fmd_cache/ { print $2 }' /proc/slabinfo`"
	touch $DIR/d36/$tfile
	sleep $((FMD_MAX_AGE + 12))
	FMD_AFTER="`awk '/ll_fmd_cache/ { print $2 }' /proc/slabinfo`"
	[ "$FMD_AFTER" -gt "$FMD_BEFORE" ] && \
		echo "AFTER : $FMD_AFTER > BEFORE $FMD_BEFORE" && \
		error "fmd didn't expire after ping" || true
}
run_test 36g "filter mod data cache expiry ====================="

test_37() {
	mkdir -p $DIR/$tdir
	echo f > $DIR/$tdir/fbugfile
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir
	ls $DIR/$tdir | grep "\<fbugfile\>" && error
	$UMOUNT $DIR/$tdir || error
	rm -f $DIR/$tdir/fbugfile || error
}
run_test 37 "ls a mounted file system to check old content ====="

test_38() {
	o_directory $DIR/$tfile
}
run_test 38 "open a regular file with O_DIRECTORY =============="

test_39() {
	touch $DIR/$tfile
	touch $DIR/${tfile}2
#	ls -l  $DIR/$tfile $DIR/${tfile}2
#	ls -lu  $DIR/$tfile $DIR/${tfile}2
#	ls -lc  $DIR/$tfile $DIR/${tfile}2
	sleep 2
	$OPENFILE -f O_CREAT:O_TRUNC:O_WRONLY $DIR/${tfile}2
	if [ ! $DIR/${tfile}2 -nt $DIR/$tfile ]; then
		echo "mtime"
		ls -l  $DIR/$tfile $DIR/${tfile}2
		echo "atime"
		ls -lu  $DIR/$tfile $DIR/${tfile}2
		echo "ctime"
		ls -lc  $DIR/$tfile $DIR/${tfile}2
		error "O_TRUNC didn't change timestamps"
	fi
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
        cat $LPROC/osc/*/stats |
            awk -vwrites=0 '/ost_write/ { writes += $2 } END { print writes; }'
}

# decent default
WRITEBACK_SAVE=500
DIRTY_RATIO_SAVE=40
MAX_DIRTY_RATIO=50
BG_DIRTY_RATIO_SAVE=10
MAX_BG_DIRTY_RATIO=25

start_writeback() {
	trap 0
	# in 2.6, restore /proc/sys/vm/dirty_writeback_centisecs,
	# dirty_ratio, dirty_background_ratio
	if [ -f /proc/sys/vm/dirty_writeback_centisecs ]; then
		echo $WRITEBACK_SAVE > /proc/sys/vm/dirty_writeback_centisecs
		echo $BG_DIRTY_RATIO_SAVE > /proc/sys/vm/dirty_background_ratio
		echo $DIRTY_RATIO_SAVE > /proc/sys/vm/dirty_ratio
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
		# save and increase /proc/sys/vm/dirty_ratio
		DIRTY_RATIO_SAVE=`cat /proc/sys/vm/dirty_ratio`
		echo $MAX_DIRTY_RATIO > /proc/sys/vm/dirty_ratio
		# save and increase /proc/sys/vm/dirty_background_ratio
		BG_DIRTY_RATIO_SAVE=`cat /proc/sys/vm/dirty_background_ratio`
		echo $MAX_BG_DIRTY_RATIO > /proc/sys/vm/dirty_background_ratio
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
	cancel_lru_locks osc
	stop_writeback
	sync; sleep 1; sync # just to be safe
	BEFOREWRITES=`count_ost_writes`
	grep "[0-9]" $LPROC/osc/*[oO][sS][cC][_-]*/cur_grant_bytes
	dd if=/dev/zero of=$DIR/f42a bs=1024 count=100
	AFTERWRITES=`count_ost_writes`
	[ $BEFOREWRITES -eq $AFTERWRITES ] || \
		error "$BEFOREWRITES < $AFTERWRITES"
	start_writeback
}
run_test 42a "ensure that we don't flush on close =============="

test_42b() {
	setup_test42
	cancel_lru_locks osc
	stop_writeback
        sync
        dd if=/dev/zero of=$DIR/f42b bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $MUNLINK $DIR/f42b || error "$MUNLINK $DIR/f42b: $?"
        AFTERWRITES=`count_ost_writes`
        if [ $BEFOREWRITES -lt $AFTERWRITES ]; then
                error "$BEFOREWRITES < $AFTERWRITES on unlink"
        fi
        BEFOREWRITES=`count_ost_writes`
        sync || error "sync: $?"
        AFTERWRITES=`count_ost_writes`
        if [ $BEFOREWRITES -lt $AFTERWRITES ]; then
                error "$BEFOREWRITES < $AFTERWRITES on sync"
        fi
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
	cancel_lru_locks osc
	stop_writeback
	# prime the file with 0,EOF PW to match
	touch $file
        $TRUNCATE $file 0
        sync; sync
	# now the real test..
        dd if=/dev/zero of=$file bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $TRUNCATE $file $offset
        cancel_lru_locks osc
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
	cp -p /bin/ls $DIR/$tdir/$tfile
	multiop $DIR/$tdir/$tfile Ow_c &
	pid=$!
	# give multiop a chance to open
	sleep 1

	$DIR/$tdir/$tfile && error || true
	kill -USR1 $pid
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
        rm $TMP/test43.junk
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
        rm $TMP/test43.junk
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
	[  "$OSTCOUNT" -lt "2" ] && skip "skipping 2-stripe test" && return
	dd if=/dev/zero of=$DIR/f1 bs=4k count=1 seek=1023
	dd if=$DIR/f1 of=/dev/null bs=4k count=1
}
run_test 44 "zero length read from a sparse stripe ============="

test_44a() {
    local nstripe=`$LCTL lov_getconfig $DIR | grep default_stripe_count: | \
                         awk '{print $2}'`
    [ -z "$nstripe" ] && skip "can't get stripe info" && return
    [ "$nstripe" -gt "$OSTCOUNT" ] && skip "Wrong default_stripe_count: $nstripe (OSTCOUNT: $OSTCOUNT)" && return
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
	for d in $LPROC/osc/*/cur_dirty_bytes; do
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
	do_dirty_record "cancel_lru_locks osc"
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
	if check_kernel_version 44; then
		touch .foo || error "'touch .foo' failed after recreating cwd"
		mkdir .bar || error "'mkdir .foo' failed after recreating cwd"
	fi
	ls . > /dev/null || error "'ls .' failed after recreating cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
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
	if check_kernel_version 44; then
		touch .foo && error "'touch .foo' worked after removing cwd"
		mkdir .foo && error "'mkdir .foo' worked after removing cwd"
	fi
	ls . > /dev/null && error "'ls .' worked after removing cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
	is_patchless || ( cd . && error "'cd .' worked after removing cwd" )
	mkdir . && error "'mkdir .' worked after removing cwd"
	rmdir . && error "'rmdir .' worked after removing cwd"
	ln -s . foo && error "'ln -s .' worked after removing cwd"
	cd .. || echo "'cd ..' failed after removing cwd `pwd`"  #bug 3517
}
run_test 48b "Access removed working dir (should return errors)="

test_48c() { # bug 2350
	check_kernel_version 36 || return 0
	#sysctl -w lnet.debug=-1
	#set -vx
	mkdir -p $DIR/d48c/dir
	cd $DIR/d48c/dir
	$TRACE rmdir $DIR/d48c/dir || error "remove cwd $DIR/d48c/dir failed"
	$TRACE touch foo && error "'touch foo' worked after removing cwd"
	$TRACE mkdir foo && error "'mkdir foo' worked after removing cwd"
	if check_kernel_version 44; then
		touch .foo && error "'touch .foo' worked after removing cwd"
		mkdir .foo && error "'mkdir .foo' worked after removing cwd"
	fi
	$TRACE ls . && error "'ls .' worked after removing cwd"
	$TRACE ls .. || error "'ls ..' failed after removing cwd"
	is_patchless || ( $TRACE cd . && error "'cd .' worked after removing cwd" )
	$TRACE mkdir . && error "'mkdir .' worked after removing cwd"
	$TRACE rmdir . && error "'rmdir .' worked after removing cwd"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing cwd"
	$TRACE cd .. || echo "'cd ..' failed after removing cwd `pwd`" #bug 3415
}
run_test 48c "Access removed working subdir (should return errors)"

test_48d() { # bug 2350
	check_kernel_version 36 || return 0
	#sysctl -w lnet.debug=-1
	#set -vx
	mkdir -p $DIR/d48d/dir
	cd $DIR/d48d/dir
	$TRACE rmdir $DIR/d48d/dir || error "remove cwd $DIR/d48d/dir failed"
	$TRACE rmdir $DIR/d48d || error "remove parent $DIR/d48d failed"
	$TRACE touch foo && error "'touch foo' worked after removing parent"
	$TRACE mkdir foo && error "'mkdir foo' worked after removing parent"
	if check_kernel_version 44; then
		touch .foo && error "'touch .foo' worked after removing parent"
		mkdir .foo && error "'mkdir .foo' worked after removing parent"
	fi
	$TRACE ls . && error "'ls .' worked after removing parent"
	$TRACE ls .. && error "'ls ..' worked after removing parent"
	is_patchless || ( $TRACE cd . && error "'cd .' worked after recreate parent" )
	$TRACE mkdir . && error "'mkdir .' worked after removing parent"
	$TRACE rmdir . && error "'rmdir .' worked after removing parent"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing parent"
	is_patchless || ( $TRACE cd .. && error "'cd ..' worked after removing parent" || true )
}
run_test 48d "Access removed parent subdir (should return errors)"

test_48e() { # bug 4134
	check_kernel_version 41 || return 0
	#sysctl -w lnet.debug=-1
	#set -vx
	mkdir -p $DIR/d48e/dir
	cd $DIR/d48e/dir
	$TRACE rmdir $DIR/d48e/dir || error "remove cwd $DIR/d48e/dir failed"
	$TRACE rmdir $DIR/d48e || error "remove parent $DIR/d48e failed"
	$TRACE touch $DIR/d48e || error "'touch $DIR/d48e' failed"
	$TRACE chmod +x $DIR/d48e || error "'chmod +x $DIR/d48e' failed"
	# On a buggy kernel addition of "touch foo" after cd .. will
	# produce kernel oops in lookup_hash_it
	touch ../foo && error "'cd ..' worked after recreate parent"
	cd $DIR
	$TRACE rm $DIR/d48e || error "rm '$DIR/d48e' failed"
}
run_test 48e "Access to recreated parent subdir (should return errors)"

test_50() {
	# bug 1485
	mkdir $DIR/d50
	cd $DIR/d50
	ls /proc/$$/cwd || error
}
run_test 50 "special situations: /proc symlinks  ==============="

test_51() {
	# bug 1516 - create an empty entry right after ".." then split dir
	mkdir $DIR/d51
	touch $DIR/d51/foo
	$MCREATE $DIR/d51/bar
	rm $DIR/d51/foo
	createmany -m $DIR/d51/longfile 201
	FNUM=202
	while [ `ls -sd $DIR/d51 | awk '{ print $1 }'` -eq 4 ]; do
		$MCREATE $DIR/d51/longfile$FNUM
		FNUM=$(($FNUM + 1))
		echo -n "+"
	done
	echo
	ls -l $DIR/d51 > /dev/null || error
}
run_test 51 "special situations: split htree with empty entry =="

export NUMTEST=70000
test_51b() {
	NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
	[ $NUMFREE -lt 21000 ] && \
		skip "not enough free inodes ($NUMFREE)" && \
		return

	check_kernel_version 40 || NUMTEST=31000
	[ $NUMFREE -lt $NUMTEST ] && NUMTEST=$(($NUMFREE - 50))

	mkdir -p $DIR/d51b
	createmany -d $DIR/d51b/t- $NUMTEST
}
run_test 51b "mkdir .../t-0 --- .../t-$NUMTEST ===================="

test_51c() {
	[ ! -d $DIR/d51b ] && skip "$DIR/51b missing" && \
		return

	unlinkmany -d $DIR/d51b/t- $NUMTEST
}
run_test 51c "rmdir .../t-0 --- .../t-$NUMTEST ===================="

test_51d() {
        [  "$OSTCOUNT" -lt "3" ] && skip "skipping test with few OSTs" && return
        mkdir -p $DIR/d51d
        createmany -o $DIR/d51d/t- 1000
        $LFS getstripe $DIR/d51d > $TMP/files
        for N in `seq 0 $((OSTCOUNT - 1))`; do
	    OBJS[$N]=`awk -vobjs=0 '($1 == '$N') { objs += 1 } END { print objs;}' $TMP/files`
	    OBJS0[$N]=`grep -A 1 idx $TMP/files | awk -vobjs=0 '($1 == '$N') { objs += 1 } END { print objs;}'`
	    log "OST$N has ${OBJS[$N]} objects, ${OBJS0[$N]} are index 0"
        done
        unlinkmany $DIR/d51d/t- 1000

        NLAST=0
        for N in `seq 1 $((OSTCOUNT - 1))`; do
	    [ ${OBJS[$N]} -lt $((${OBJS[$NLAST]} - 20)) ] && \
		error "OST $N has less objects vs OST $NLAST (${OBJS[$N]} < ${OBJS[$NLAST]}"
	    [ ${OBJS[$N]} -gt $((${OBJS[$NLAST]} + 20)) ] && \
		error "OST $N has less objects vs OST $NLAST (${OBJS[$N]} < ${OBJS[$NLAST]}"
	    
	    [ ${OBJS0[$N]} -lt $((${OBJS0[$NLAST]} - 20)) ] && \
		error "OST $N has less #0 objects vs OST $NLAST (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
	    [ ${OBJS0[$N]} -gt $((${OBJS0[$NLAST]} + 20)) ] && \
		error "OST $N has less #0 objects vs OST $NLAST (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
	    NLAST=$N
        done
}
run_test 51d "check object distribution ===================="

test_52a() {
	[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
	mkdir -p $DIR/d52a
	touch $DIR/d52a/foo
	chattr =a $DIR/d52a/foo || error "chattr =a failed"
	echo bar >> $DIR/d52a/foo || error "append bar failed"
	cp /etc/hosts $DIR/d52a/foo && error "cp worked"
	rm -f $DIR/d52a/foo 2>/dev/null && error "rm worked"
	link $DIR/d52a/foo $DIR/d52a/foo_link 2>/dev/null && error "link worked"
	echo foo >> $DIR/d52a/foo || error "append foo failed"
	mrename $DIR/d52a/foo $DIR/d52a/foo_ren && error "rename worked"
	lsattr $DIR/d52a/foo | egrep -q "^-+a-+ $DIR/d52a/foo" || error "lsattr"
	chattr -a $DIR/d52a/foo || error "chattr -a failed"

	rm -fr $DIR/d52a || error "cleanup rm failed"
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

test_52c() { # 12848 simulating client < 1.4.7
        [ -f $DIR/d52c/foo ] && chattr -i $DIR/d52b/foo
        mkdir -p $DIR/d52c
        touch $DIR/d52c/foo
        # skip MDS_BFLAG_EXT_FLAGS in mdc_getattr_pack
#define OBD_FAIL_MDC_OLD_EXT_FLAGS       0x802
        sysctl -w lustre.fail_loc=0x802
        chattr =i $DIR/d52c/foo || error
        lsattr $DIR/d52c/foo | egrep -q "^-+i-+ $DIR/d52c/foo" || error
        chattr -i $DIR/d52c/foo || error
        sysctl -w lustre.fail_loc=0

        rm -fr $DIR/d52c || error
}
run_test 52c "immutable flag test for client < 1.4.7 ======="

test_53() {
	remote_mds && skip "remote MDS" && return
	
        for i in `ls -d $LPROC/osc/*-osc 2> /dev/null` ; do
                ostname=`basename $i | cut -d - -f 1-2`
                ost_last=`cat $LPROC/obdfilter/$ostname/last_id`
                mds_last=`cat $i/prealloc_last_id`
                echo "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                if [ $ost_last != $mds_last ]; then
                    error "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                fi
        done
}
run_test 53 "verify that MDS and OSTs agree on pre-creation ===="

test_54a() {
        [ ! -f "$SOCKETSERVER" ] && skip "no socketserver, skipping" && return
        [ ! -f "$SOCKETCLIENT" ] && skip "no socketclient, skipping" && return
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
	$UMOUNT $tdir
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

test_54e() {
	check_kernel_version 46 || return 0
	f="$DIR/f54e"
	string="aaaaaa"
	cp -aL /dev/console $f
	echo $string > $f || error
}
run_test 54e "console/tty device works in lustre ======================"

check_fstype() {
	grep -q $FSTYPE /proc/filesystems && return 1
	modprobe $FSTYPE
	grep -q $FSTYPE /proc/filesystems && return 1
	insmod ../$FSTYPE/$FSTYPE.o
	grep -q $FSTYPE /proc/filesystems && return 1
	insmod ../$FSTYPE/$FSTYPE.ko
	grep -q $FSTYPE /proc/filesystems && return 1
	return 0
}

test_55() {
        rm -rf $DIR/d55
        mkdir $DIR/d55
        check_fstype && skip "can't find fs $FSTYPE" && return
        mount -t $FSTYPE -o loop,iopen $EXT2_DEV $DIR/d55 || error "mounting"
        touch $DIR/d55/foo
        $IOPENTEST1 $DIR/d55/foo $DIR/d55 || error "running $IOPENTEST1"
        $IOPENTEST2 $DIR/d55 || error "running $IOPENTEST2"
        echo "check for $EXT2_DEV. Please wait..."
        rm -rf $DIR/d55/*
        $UMOUNT $DIR/d55 || error "unmounting"
}
run_test 55 "check iopen_connect_dentry() ======================"

test_56() {
        rm -rf $DIR/d56
        $SETSTRIPE -d $DIR
        mkdir $DIR/d56
        mkdir $DIR/d56/dir
        NUMFILES=3
        NUMFILESx2=$(($NUMFILES * 2))
        for i in `seq 1 $NUMFILES` ; do
                touch $DIR/d56/file$i
                touch $DIR/d56/dir/file$i
        done

        # test lfs getstripe with --recursive
        FILENUM=`$GETSTRIPE --recursive $DIR/d56 | grep -c obdidx`
        [ $FILENUM -eq $NUMFILESx2 ] || error \
                "lfs getstripe --recursive $DIR/d56 wrong: found $FILENUM, expected $NUMFILESx2"
        FILENUM=`$GETSTRIPE $DIR/d56 | grep -c obdidx`
        [ $FILENUM -eq $NUMFILES ] || error \
                "lfs getstripe $DIR/d56 without --recursive wrong: found $FILENUM, expected $NUMFILES"
        echo "lfs getstripe --recursive passed."

        # test lfs getstripe with file instead of dir
        FILENUM=`$GETSTRIPE $DIR/d56/file1 | grep -c obdidx`
        [ $FILENUM  -eq 1 ] || error \
                 "lfs getstripe $DIR/d56/file1 wrong:found $FILENUM, expected 1"
        echo "lfs getstripe file passed."

        #test lfs getstripe with --verbose
        [ `$GETSTRIPE --verbose $DIR/d56 | grep -c lmm_magic` -eq $NUMFILES ] ||\
                error "lfs getstripe --verbose $DIR/d56 wrong: should find $NUMFILES lmm_magic info"
        [ `$GETSTRIPE $DIR/d56 | grep -c lmm_magic` -eq 0 ] || error \
                "lfs getstripe $DIR/d56 without --verbose wrong: should not show lmm_magic info"
        echo "lfs getstripe --verbose passed."

        #test lfs getstripe with --obd
        $GETSTRIPE --obd wrong_uuid $DIR/d56 2>&1 | grep -q "unknown obduuid" || \
                error "lfs getstripe --obd wrong_uuid should return error message"

        [  "$OSTCOUNT" -lt 2 ] && \
                skip "skipping other lfs getstripe --obd test" && return
        FILENUM=`$GETSTRIPE --recursive $DIR/d56 | sed -n '/^[	 ]*1[	 ]/p' | wc -l`
        OBDUUID=`$GETSTRIPE --recursive $DIR/d56 | sed -n '/^[	 ]*1:/p' | awk '{print $2}'`
        FOUND=`$GETSTRIPE -r --obd $OBDUUID $DIR/d56 | wc -l`
        [ $FOUND -eq $FILENUM ] || \
                error "lfs getstripe --obd wrong: found $FOUND, expected $FILENUM"
        [ `$GETSTRIPE -r -v --obd $OBDUUID $DIR/d56 | sed '/^[	 ]*1[	 ]/d' |\
                sed -n '/^[	 ]*[0-9][0-9]*[	 ]/p' | wc -l` -eq 0 ] || \
                error "lfs getstripe --obd wrong: should not show file on other obd"
        echo "lfs getstripe --obd passed."
}
run_test 56 "check lfs getstripe ===================================="

NUMFILES=3
NUMDIRS=3
setup_56() {
        LOCAL_NUMFILES=$1
        LOCAL_NUMDIRS=$2
        if [ ! -d "$DIR/${tdir}g" ] ; then
                mkdir -p $DIR/${tdir}g
                for i in `seq 1 $LOCAL_NUMFILES` ; do
                        touch $DIR/${tdir}g/file$i
                done
                for i in `seq 1 $LOCAL_NUMDIRS` ; do
                        mkdir $DIR/${tdir}g/dir$i
                        for j in `seq 1 $LOCAL_NUMFILES` ; do
                                touch $DIR/${tdir}g/dir$i/file$j
                        done
                done
        fi
}

setup_56_special() {
	LOCAL_NUMFILES=$1
	LOCAL_NUMDIRS=$2
	TDIR=$DIR/${tdir}g
	setup_56 $1 $2
	if [ ! -e "$TDIR/loop1b" ] ; then
		for i in `seq 1 $LOCAL_NUMFILES` ; do
			mknod $TDIR/loop${i}b b 7 $i
			mknod $TDIR/null${i}c c 1 3
			ln -s $TDIR/file1 $TDIR/link${i}l
		done
		for i in `seq 1 $LOCAL_NUMDIRS` ; do
			mknod $TDIR/dir$i/loop${i}b b 7 $i
			mknod $TDIR/dir$i/null${i}c c 1 3
			ln -s $TDIR/dir$i/file1 $TDIR/dir$i/link${i}l
		done
	fi
}

test_56g() {
        $LSTRIPE -d $DIR

        setup_56 $NUMFILES $NUMDIRS

        EXPECTED=$(($NUMDIRS + 2))
        # test lfs find with -name
        for i in `seq 1 $NUMFILES` ; do
                NUMS=`$LFIND -name "*$i" $DIR/${tdir}g | wc -l`
                [ $NUMS -eq $EXPECTED ] || error \
                        "lfs find -name \"*$i\" $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
        done
        echo "lfs find -name passed."
}
run_test 56g "check lfs find -name ============================="

test_56h() {
        $LSTRIPE -d $DIR

        setup_56 $NUMFILES $NUMDIRS

        EXPECTED=$((($NUMDIRS+1)*($NUMFILES-1)+$NUMFILES))
        # test lfs find with ! -name
        for i in `seq 1 $NUMFILES` ; do
                NUMS=`$LFIND ! -name "*$i" $DIR/${tdir}g | wc -l`
                [ $NUMS -eq $EXPECTED ] || error \
                        "lfs find ! -name \"*$i\" $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
        done
        echo "lfs find ! -name passed."
}
run_test 56h "check lfs find ! -name ============================="

test_56i() {
       tdir=${tdir}i
       mkdir -p $DIR/$tdir
       UUID=`$GETSTRIPE $DIR/$tdir | awk '/0: / { print $2 }'`
       OUT="`$LFIND -ost $UUID $DIR/$tdir`"
       [ "$OUT" ] && error "$LFIND returned directory '$OUT'" || true
}
run_test 56i "check 'lfs find -ost UUID' skips directories ======="

test_56j() {
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS+1))
	NUMS=`$LFIND -type d $DIR/${tdir}g | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -type d $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
}
run_test 56j "check lfs find -type d ============================="

test_56k() {
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$(((NUMDIRS+1) * NUMFILES))
	NUMS=`$LFIND -type f $DIR/${tdir}g | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -type f $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
}
run_test 56k "check lfs find -type f ============================="

test_56l() {
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	NUMS=`$LFIND -type b $DIR/${tdir}g | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -type b $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
}
run_test 56l "check lfs find -type b ============================="

test_56m() {
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	NUMS=`$LFIND -type c $DIR/${tdir}g | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -type c $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
}
run_test 56m "check lfs find -type c ============================="

test_56n() {
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	NUMS=`$LFIND -type l $DIR/${tdir}g | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -type l $DIR/${tdir}g wrong: found $NUMS, expected $EXPECTED"
}
run_test 56n "check lfs find -type l ============================="

test_56o() {
	setup_56 $NUMFILES $NUMDIRS
	TDIR=$DIR/${tdir}g

	utime $TDIR/file1 > /dev/null || error
	utime $TDIR/file2 > /dev/null || error
	utime $TDIR/dir1 > /dev/null || error
	utime $TDIR/dir2 > /dev/null || error
	utime $TDIR/dir1/file1 > /dev/null || error

	EXPECTED=5
	NUMS=`$LFIND -mtime +1 $TDIR | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -mtime $TDIR wrong: found $NUMS, expected $EXPECTED"
}
run_test 56o "check lfs find -mtime for old files =========================="

test_57a() {
	remote_mds && skip "remote MDS" && return

	for DEV in `cat $LPROC/mds/*/mntdev`; do
		dumpe2fs -h $DEV > $TMP/t57a.dump || error "can't access $DEV"
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
	$GETSTRIPE $FILE1 2>&1 | grep -q "no stripe" || error "$FILE1 has an EA"
	$GETSTRIPE $FILEN 2>&1 | grep -q "no stripe" || error "$FILEN has an EA"

	MDSFREE="`cat $LPROC/mds/*/kbytesfree 2> /dev/null`"
	MDCFREE="`cat $LPROC/mdc/*/kbytesfree | head -n 1`"
	echo "opening files to create objects/EAs"
	for FILE in `seq -f $DIR/d57b/f%g 1 $FILECOUNT`; do
		$OPENFILE -f O_RDWR $FILE > /dev/null || error "opening $FILE"
	done

	# verify that files have EAs now
	$GETSTRIPE $FILE1 | grep -q "obdidx" || error "$FILE1 missing EA"
	$GETSTRIPE $FILEN | grep -q "obdidx" || error "$FILEN missing EA"

	sleep 1 # make sure we get new statfs data
	MDSFREE2="`cat $LPROC/mds/*/kbytesfree 2> /dev/null`"
	MDCFREE2="`cat $LPROC/mdc/*/kbytesfree | head -n 1`"
	if [ "$MDCFREE2" -lt "$((MDCFREE - 8))" ]; then
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
    [ -z "$(which wiretest 2>/dev/null)" ] && skip "could not find wiretest" && return
    wiretest
}
run_test 58 "verify cross-platform wire constants =============="

test_59() {
	echo "touch 130 files"
	createmany -o $DIR/f59- 130
	echo "rm 130 files"
	unlinkmany $DIR/f59- 130
	sync
	sleep 2
        # wait for commitment of removal
}
run_test 59 "verify cancellation of llog records async ========="

TEST60_HEAD="test_60 run $RANDOM"
test_60() {
        [ ! -f run-llog.sh ] && skip "missing subtest run-llog.sh" && return
	log "$TEST60_HEAD - from kernel mode"
	sh run-llog.sh
}
run_test 60 "llog sanity tests run from kernel module =========="

test_60b() { # bug 6411
	dmesg > $DIR/$tfile
	LLOG_COUNT=`dmesg | awk "/$TEST60_HEAD/{marker = 1; from_marker = 0;}
				 /llog-test/ {
					 if (marker)
						 from_marker++
					 from_begin++
				 }
				 END {
					 if (marker)
						 print from_marker
					 else
						 print from_begin
				 }"`
	[ $LLOG_COUNT -gt 50 ] && error "CDEBUG_LIMIT not limiting messages ($LLOG_COUNT)"|| true
}
run_test 60b "limit repeated messages from CERROR/CWARN ========"

test_60c() {
	echo "create 5000 files" 
	createmany -o $DIR/f60c- 5000
	#define OBD_FAIL_MDS_LLOG_CREATE_FAILED  0x137
	sysctl -w lustre.fail_loc=0x80000137
	unlinkmany $DIR/f60c- 5000
}
run_test 60c "unlink file when mds full"

test_60d() {
	SAVEPRINTK=$(sysctl -n lnet.printk)

	# verify "lctl mark" is even working"
	MESSAGE="test message ID $RANDOM $$"
	$LCTL mark "$MESSAGE" || error "$LCTL mark failed"
	dmesg | grep -q "$MESSAGE" || error "didn't find debug marker in log"

	sysctl -w lnet.printk=0 || error "set lnet.printk failed"
	sysctl -n lnet.printk | grep emerg || error "lnet.printk dropped emerg"

	MESSAGE="new test message ID $RANDOM $$"
	# Assume here that libcfs_debug_mark_buffer() uses D_WARNING
	$LCTL mark "$MESSAGE" || error "$LCTL mark failed"
	dmesg | grep -q "$MESSAGE" && error "D_WARNING wasn't masked" || true

	sysctl -w lnet.printk="$SAVEPRINTK"
}
run_test 60d "test printk console message masking"

test_61() {
	f="$DIR/f61"
	dd if=/dev/zero of=$f bs=`page_size` count=1
	cancel_lru_locks osc
	multiop $f OSMWUc || error
	sync
}
run_test 61 "mmap() writes don't make sync hang ================"

# bug 2330 - insufficient obd_match error checking causes LBUG
test_62() {
        f="$DIR/f62"
        echo foo > $f
        cancel_lru_locks osc
        #define OBD_FAIL_OSC_MATCH 0x405
        sysctl -w lustre.fail_loc=0x405
        cat $f && error "cat succeeded, expect -EIO"
        sysctl -w lustre.fail_loc=0
}
# This test is now irrelevant (as of bug 10718 inclusion), we no longer
# match every page all of the time.
#run_test 62 "verify obd_match failure doesn't LBUG (should -EIO)"

# bug 2319 - oig_wait() interrupted causes crash because of invalid waitq.
test_63() {
	MAX_DIRTY_MB=`cat $LPROC/osc/*/max_dirty_mb | head -n 1`
	for i in $LPROC/osc/*/max_dirty_mb ; do
		echo 0 > $i
	done
	for i in `seq 10` ; do
		dd if=/dev/zero of=$DIR/f63 bs=8k &
		sleep 5
		kill $!
		sleep 1
	done

	for i in $LPROC/osc/*/max_dirty_mb ; do
		echo $MAX_DIRTY_MB > $i
	done
	rm -f $DIR/f63 || true
}
run_test 63 "Verify oig_wait interruption does not crash ======="

# bug 2248 - async write errors didn't return to application on sync
# bug 3677 - async write errors left page locked
test_63b() {
	debugsave
	sysctl -w lnet.debug=-1

	# ensure we have a grant to do async writes
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=1
	rm $DIR/$tfile

	#define OBD_FAIL_OSC_BRW_PREP_REQ 0x406
	sysctl -w lustre.fail_loc=0x80000406
	multiop $DIR/$tfile Owy && \
		error "sync didn't return ENOMEM"
	sync; sleep 2; sync	# do a real sync this time to flush page
	grep locked $LPROC/llite/*/dump_page_cache && \
		error "locked page left in cache after async error" || true
	debugrestore
}
run_test 63b "async write errors should be returned to fsync ==="

test_64a () {
	df $DIR
	grep "[0-9]" $LPROC/osc/*[oO][sS][cC][_-]*/cur*
}
run_test 64a "verify filter grant calculations (in kernel) ====="

test_64b () {
        [ ! -f oos.sh ] && skip "missing subtest oos.sh" && return
	sh oos.sh $MOUNT
}
run_test 64b "check out-of-space detection on client ==========="

# bug 1414 - set/get directories' stripe info
test_65a() {
	mkdir -p $DIR/d65
	touch $DIR/d65/f1
	$LVERIFY $DIR/d65 $DIR/d65/f1 || error "lverify failed"
}
run_test 65a "directory with no stripe info ===================="

test_65b() {
	mkdir -p $DIR/d65
	$SETSTRIPE $DIR/d65 -s $(($STRIPESIZE * 2)) -i 0 -c 1 || error "setstripe"
	touch $DIR/d65/f2
	$LVERIFY $DIR/d65 $DIR/d65/f2 || error "lverify failed"
}
run_test 65b "directory setstripe $(($STRIPESIZE * 2)) 0 1 ==============="

test_65c() {
	if [ $OSTCOUNT -gt 1 ]; then
		mkdir -p $DIR/d65
    		$SETSTRIPE $DIR/d65 -s $(($STRIPESIZE * 4)) -i 1 \
			-c $(($OSTCOUNT - 1)) || error "setstripe"
		touch $DIR/d65/f3
		$LVERIFY $DIR/d65 $DIR/d65/f3 || error "lverify failed"
	fi
}
run_test 65c "directory setstripe $(($STRIPESIZE * 4)) 1 $(($OSTCOUNT - 1))"

test_65d() {
	mkdir -p $DIR/d65
	[ $STRIPECOUNT -le 0 ] && sc=1 || sc=$(($STRIPECOUNT - 1))
	$SETSTRIPE $DIR/d65 -s $STRIPESIZE -c $sc || error "setstripe"
	touch $DIR/d65/f4 $DIR/d65/f5
	$LVERIFY $DIR/d65 $DIR/d65/f4 $DIR/d65/f5 || error "lverify failed"
}
run_test 65d "directory setstripe $STRIPESIZE -1 stripe_count =============="

test_65e() {
	mkdir -p $DIR/d65

	$SETSTRIPE $DIR/d65 || error "setstripe"
        $GETSTRIPE -v $DIR/d65 | grep "Default" || error "no stripe info failed"
	touch $DIR/d65/f6
	$LVERIFY $DIR/d65 $DIR/d65/f6 || error "lverify failed"
}
run_test 65e "directory setstripe 0 -1 0 ======================="

test_65f() {
	mkdir -p $DIR/d65f
	$RUNAS $SETSTRIPE $DIR/d65f && error "setstripe succeeded" || true
}
run_test 65f "dir setstripe permission (should return error) ==="

test_65g() {
        mkdir -p $DIR/d65
        $SETSTRIPE $DIR/d65 -s $(($STRIPESIZE * 2)) -i 0 -c 1 || error "setstripe"
        $SETSTRIPE -d $DIR/d65 || error "setstripe"
        $GETSTRIPE -v $DIR/d65 | grep "Default" || \
		error "delete default stripe failed"
}
run_test 65g "directory setstripe -d ==========================="

test_65h() {
        mkdir -p $DIR/d65
        $SETSTRIPE $DIR/d65 -s $(($STRIPESIZE * 2)) -i 0 -c 1 || error "setstripe"
        mkdir -p $DIR/d65/dd1
        [ "`$GETSTRIPE -v $DIR/d65 | grep "^count"`" == \
          "`$GETSTRIPE -v $DIR/d65/dd1 | grep "^count"`" ] || error "stripe info inherit failed"
}
run_test 65h "directory stripe info inherit ===================="
 
test_65i() { # bug6367
        $SETSTRIPE $MOUNT -s 65536 -c -1 
}
run_test 65i "set non-default striping on root directory (bug 6367)="

test_65j() { # bug6367
	# if we aren't already remounting for each test, do so for this test
	if [ "$CLEANUP" = ":" -a "$I_MOUNTED" = "yes" ]; then
		cleanup -f || error "failed to unmount"
		setup
	fi
	$SETSTRIPE -d $MOUNT || error "setstripe failed"
}
run_test 65j "set default striping on root directory (bug 6367)="

test_65k() { # bug11679
        [ "$OSTCOUNT" -lt 2 ] && skip "too few OSTs" && return

        echo "Check OST status: "
        MDS_OSCS=`do_facet mds lctl dl | awk '/[oO][sS][cC].*md[ts]/ { print $4 }'`
        for OSC in $MDS_OSCS; do
                echo $OSC "is activate"
                do_facet mds lctl --device %$OSC activate
        done
        do_facet client mkdir -p $DIR/$tdir
        for INACTIVE_OSC in $MDS_OSCS; do
                echo $INACTIVE_OSC "is Deactivate:"
                do_facet mds lctl --device  %$INACTIVE_OSC deactivate
                for STRIPE_OSC in $MDS_OSCS; do
                        STRIPE_OST=`osc_to_ost $STRIPE_OSC`
                        STRIPE_INDEX=`do_facet mds cat $LPROC/lov/*md*/target_obd |
                                      grep $STRIPE_OST | awk -F: '{print $1}'`
                        echo "$SETSTRIPE $DIR/$tdir/${STRIPE_INDEX} -i ${STRIPE_INDEX} -c 1"
                        do_facet client $SETSTRIPE $DIR/$tdir/${STRIPE_INDEX} -i ${STRIPE_INDEX} -c 1
                        RC=$?
                        [ $RC -ne 0 ] && error "setstripe should have succeeded"
                done
                do_facet client rm -f $DIR/$tdir/*
                echo $INACTIVE_OSC "is Activate."
                do_facet mds lctl --device  %$INACTIVE_OSC activate
        done
}
run_test 65k "validate manual striping works properly with deactivated OSCs"

test_65l() { # bug 12836
	mkdir -p $DIR/$tdir
	$SETSTRIPE $DIR/$tdir -c -1
	$LFS find -mtime -1 $DIR >/dev/null
}
run_test 65l "lfs find on -1 stripe dir ========================"

# bug 2543 - update blocks count on client
test_66() {
	COUNT=${COUNT:-8}
	dd if=/dev/zero of=$DIR/f66 bs=1k count=$COUNT
	sync; sleep 1; sync
	BLOCKS=`ls -s $DIR/f66 | awk '{ print $1 }'`
	[ $BLOCKS -ge $COUNT ] || error "$DIR/f66 blocks $BLOCKS < $COUNT"
}
run_test 66 "update inode blocks count on client ==============="

test_67() { # bug 3285 - supplementary group fails on MDS, passes on client
	[ "$RUNAS_ID" = "$UID" ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	check_kernel_version 35 || return 0
	mkdir $DIR/$tdir
	chmod 771 $DIR/$tdir
	chgrp $RUNAS_ID $DIR/$tdir
	$RUNAS -u $RUNAS_ID -g $(($RUNAS_ID + 1)) -G1,2,$RUNAS_ID ls $DIR/$tdir
	RC=$?
	MDS=$(\ls $LPROC/mds 2> /dev/null | grep -v num_refs | tail -n 1)
	if [ "$MDS" ]; then
		# can't tell which is correct otherwise
		GROUP_UPCALL=`cat $LPROC/mds/$MDS/group_upcall`
		[ "$GROUP_UPCALL" = "NONE" -a $RC -eq 0 ] && \
			error "no-upcall passed" || true
		[ "$GROUP_UPCALL" != "NONE" -a $RC -ne 0 ] && \
			error "upcall failed" || true
	fi
}
run_test 67 "supplementary group failure (should return error) ="

cleanup_67b() {
	set +vx
	trap 0
	echo NONE > $LPROC/mds/$MDS/group_upcall
}

test_67b() { # bug 3285 - supplementary group fails on MDS, passes on client
	T67_UID=${T67_UID:-1}	# needs to be in /etc/groups on MDS, gid == uid
	[ "$UID" = "$T67_UID" ] && skip "UID = T67_UID = $UID -- skipping" && return
	check_kernel_version 35 || return 0
	remote_mds && skip "remote MDS" && return
	GROUP_UPCALL=`cat $LPROC/mds/$MDS/group_upcall`
	[ "$GROUP_UPCALL" != "NONE" ] && skip "skip test - upcall" &&return
	set -vx
	trap cleanup_67b EXIT
	mkdir -p $DIR/$tdir
	chmod 771 $DIR/$tdir
	chgrp $T67_UID $DIR/$tdir
	echo `which l_getgroups` > $LPROC/mds/$MDS/group_upcall
	l_getgroups -d $T67_UID
	$RUNAS -u $T67_UID -g 999 -G8,9,$T67_UID touch $DIR/$tdir/$tfile || \
		error "'touch $DIR/$tdir/$tfile' failed"
	[ -f $DIR/$tdir/$tfile ] || error "$DIR/$tdir/$tfile create error"
	cleanup_67b
}
run_test 67b "supplementary group test ========================="

LLOOP=
cleanup_68() {
	trap 0
	if [ ! -z "$LLOOP" ]; then
		swapoff $LLOOP || error "swapoff failed"
		$LCTL blockdev_detach $LLOOP || error "detach failed"
		rm -f $LLOOP
		unset LLOOP
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
	[ "$UID" != 0 ] && skip "must run as root" && return
	grep -q obdfilter $LPROC/devices && \
		skip "local OST" && return

	grep -q llite_lloop /proc/modules
	[ $? -ne 0 ] && skip "can't find module llite_lloop" && return

	[ -z "`$LCTL list_nids | grep -v tcp`" ] && \
		skip "can't reliably test swap with TCP" && return

	MEMTOTAL=`meminfo MemTotal`
	NR_BLOCKS=$((MEMTOTAL>>8))
	[[ $NR_BLOCKS -le 2048 ]] && NR_BLOCKS=2048

	LLOOP=$TMP/lloop.`date +%s`.`date +%N`
	dd if=/dev/zero of=$DIR/f68 bs=64k seek=$NR_BLOCKS count=1
	mkswap $DIR/f68

	$LCTL blockdev_attach $DIR/f68 $LLOOP || error "attach failed"

	trap cleanup_68 EXIT

	swapon -p 32767 $LLOOP || error "swapon $LLOOP failed"

	echo "before: `swapon -s | grep $LLOOP`"
	$MEMHOG $MEMTOTAL || error "error allocating $MEMTOTAL kB"
	echo "after: `swapon -s | grep $LLOOP`"
	SWAPUSED=`swap_used $LLOOP`

	cleanup_68

	[ $SWAPUSED -eq 0 ] && echo "no swap used???" || true
}
run_test 68 "support swapping to Lustre ========================"

# bug5265, obdfilter oa2dentry return -ENOENT
# #define OBD_FAIL_OST_ENOENT 0x217
test_69() {
	[ $(grep -c obdfilter $LPROC/devices) -eq 0 ] &&
		skip "skipping test for remote OST" && return

	f="$DIR/$tfile"
	touch $f

	if ! $DIRECTIO write ${f}.2 0 1; then
		skip "O_DIRECT not implemented"
		return 0
	fi

	#define OBD_FAIL_OST_ENOENT 0x217
	sysctl -w lustre.fail_loc=0x217
	truncate $f 1 # vmtruncate() will ignore truncate() error.
	$DIRECTIO write $f 0 2 && error "write succeeded, expect -ENOENT"

	sysctl -w lustre.fail_loc=0
	$DIRECTIO write $f 0 2 || error "write error"

	cancel_lru_locks osc
	$DIRECTIO read $f 0 1 || error "read error"

	#define OBD_FAIL_OST_ENOENT 0x217
	sysctl -w lustre.fail_loc=0x217
	$DIRECTIO read $f 1 1 && error "read succeeded, expect -ENOENT"

	sysctl -w lustre.fail_loc=0
	rm -f $f
}
run_test 69 "verify oa2dentry return -ENOENT doesn't LBUG ======"

test_71() {
        which dbench > /dev/null 2>&1 || skip "dbench not installed, skip this test" && return 0
	DBENCH_LIB=${DBENCH_LIB:-/usr/lib/dbench}
	PATH=${DBENCH_LIB}:${PATH}
	cp `which dbench` $DIR

	TGT=$DIR/client.txt
	SRC=${SRC:-$DBENCH_LIB/client.txt}
	[ ! -e $TGT -a -e $SRC ] && echo "copying $SRC to $TGT" && cp $SRC $TGT
	SRC=$DBENCH_LIB/client_plain.txt
	[ ! -e $TGT -a -e $SRC ] && echo "copying $SRC to $TGT" && cp $SRC $TGT

	echo "copying necessary lib to $DIR"
	[ -d /lib64 ] && LIB71=/lib64 || LIB71=/lib
	mkdir -p $DIR$LIB71 || error "can't create $DIR$LIB71"
	cp $LIB71/libc* $DIR$LIB71 || error "can't copy $LIB71/libc*"
	cp $LIB71/ld-* $DIR$LIB71 || error "can't create $LIB71/ld-*"

	echo "chroot $DIR /dbench -c client.txt 2"
	chroot $DIR /dbench -c client.txt 2
	RC=$?

	rm -rf $DIR/dbench $TGT $DIR$LIB71

	return $RC
}
run_test 71 "Running dbench on lustre (don't segment fault) ===="

test_72() { # bug 5695 - Test that on 2.6 remove_suid works properly
	check_kernel_version 43 || return 0
	[ "$RUNAS_ID" = "$UID" ] && skip "RUNAS_ID = UID = $UID -- skipping" && return
	touch $DIR/f72
	chmod 777 $DIR/f72
	chmod ug+s $DIR/f72
	$RUNAS dd if=/dev/zero of=$DIR/f72 bs=512 count=1 || error
	# See if we are still setuid/sgid
	test -u $DIR/f72 -o -g $DIR/f72 && error "S/gid is not dropped on write"
	# Now test that MDS is updated too
	cancel_lru_locks mdc
	test -u $DIR/f72 -o -g $DIR/f72 && error "S/gid is not dropped on MDS"
	true
}
run_test 72 "Test that remove suid works properly (bug5695) ===="

# bug 3462 - multiple simultaneous MDC requests
test_73() {
	mkdir $DIR/d73-1 
	mkdir $DIR/d73-2
	multiop $DIR/d73-1/f73-1 O_c &
	pid1=$!
	#give multiop a chance to open
	usleep 500

	#define OBD_FAIL_MDS_PAUSE_OPEN 0x129
	sysctl -w lustre.fail_loc=0x80000129
	multiop $DIR/d73-1/f73-2 Oc &
	sleep 1
	sysctl -w lustre.fail_loc=0

	multiop $DIR/d73-2/f73-3 Oc &
	pid3=$!

	kill -USR1 $pid1
	wait $pid1 || return 1

	sleep 25

	$CHECKSTAT -t file $DIR/d73-1/f73-1 || return 4
	$CHECKSTAT -t file $DIR/d73-1/f73-2 || return 5 
	$CHECKSTAT -t file $DIR/d73-2/f73-3 || return 6 

	rm -rf $DIR/d73-*
}
run_test 73 "multiple MDC requests (should not deadlock)"

test_74a() { # bug 6149, 6184
	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	touch $DIR/f74a
	sysctl -w lustre.fail_loc=0x8000030e
	# get any lock that won't be difficult - lookup works.
	ls $DIR/f74a
	sysctl -w lustre.fail_loc=0
	true
}
run_test 74a "ldlm_enqueue freed-export error path, ls (shouldn't LBUG)"

test_74b() { # bug 13310
	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	sysctl -w lustre.fail_loc=0x8000030e
	# get a "difficult" lock
	touch $DIR/f74b
	sysctl -w lustre.fail_loc=0
	true
}
run_test 74b "ldlm_enqueue freed-export error path, touch (shouldn't LBUG)"

JOIN=${JOIN:-"lfs join"}
F75=$DIR/f75
F128k=${F75}_128k
FHEAD=${F75}_head
FTAIL=${F75}_tail
export T75_PREP=no
test75_prep() {
        [ $T75_PREP = "yes" ] && return
        echo "using F75=$F75, F128k=$F128k, FHEAD=$FHEAD, FTAIL=$FTAIL"
 
        dd if=/dev/urandom of=${F75}_128k bs=128k count=1 || error "dd failed"
        log "finished dd"
        chmod 777 ${F128k}
        T75_PREP=yes
}
 
test_75a() {
        test75_prep
 
        cp -p ${F128k} ${FHEAD}
        log "finished cp to $FHEAD"
        cp -p ${F128k} ${FTAIL}
        log "finished cp to $FTAIL"
        cat ${F128k} ${F128k} > ${F75}_sim_sim
 
        $JOIN ${FHEAD} ${FTAIL} || error "join ${FHEAD} ${FTAIL} error"
        log "finished join $FHEAD to ${F75}_sim_sim"
        cmp ${FHEAD} ${F75}_sim_sim || error "${FHEAD} ${F75}_sim_sim differ"
        log "finished cmp $FHEAD to ${F75}_sim_sim"
        $CHECKSTAT -a ${FTAIL} || error "tail ${FTAIL} still exist after join"
}
run_test 75a "TEST join file ===================================="
 
test_75b() {
        test75_prep
 
        cp -p ${F128k} ${FTAIL}
        cat ${F75}_sim_sim >> ${F75}_join_sim
        cat ${F128k} >> ${F75}_join_sim
        $JOIN ${FHEAD} ${FTAIL} || error "join ${FHEAD} ${FTAIL} error"
        cmp ${FHEAD} ${F75}_join_sim || \
                error "${FHEAD} ${F75}_join_sim are different"
        $CHECKSTAT -a ${FTAIL} || error "tail ${FTAIL} exist after join"
}
run_test 75b "TEST join file 2 =================================="
 
test_75c() {
        test75_prep
 
        cp -p ${F128k} ${FTAIL}
        cat ${F128k} >> ${F75}_sim_join
        cat ${F75}_join_sim >> ${F75}_sim_join
        $JOIN ${FTAIL} ${FHEAD} || error "join error"
        cmp ${FTAIL} ${F75}_sim_join || \
                error "${FTAIL} ${F75}_sim_join are different"
        $CHECKSTAT -a ${FHEAD} || error "tail ${FHEAD} exist after join"
}
run_test 75c "TEST join file 3 =================================="
 
test_75d() {
        test75_prep
 
        cp -p ${F128k} ${FHEAD}
        cp -p ${F128k} ${FHEAD}_tmp
        cat ${F75}_sim_sim >> ${F75}_join_join
        cat ${F75}_sim_join >> ${F75}_join_join
        $JOIN ${FHEAD} ${FHEAD}_tmp || error "join ${FHEAD} ${FHEAD}_tmp error"
        $JOIN ${FHEAD} ${FTAIL} || error "join ${FHEAD} ${FTAIL} error"
        cmp ${FHEAD} ${F75}_join_join ||error "${FHEAD} ${F75}_join_join differ"        $CHECKSTAT -a ${FHEAD}_tmp || error "${FHEAD}_tmp exist after join"
        $CHECKSTAT -a ${FTAIL} || error "tail ${FTAIL} exist after join (2)"
}
run_test 75d "TEST join file 4 =================================="
 
test_75e() {
        test75_prep
 
        rm -rf ${FHEAD} || "delete join file error"
}
run_test 75e "TEST join file 5 (remove joined file) ============="
 
test_75f() {
        test75_prep
 
        cp -p ${F128k} ${F75}_join_10_compare
        cp -p ${F128k} ${F75}_join_10
        for ((i = 0; i < 10; i++)); do
                cat ${F128k} >> ${F75}_join_10_compare
                cp -p ${F128k} ${FTAIL}
                $JOIN ${F75}_join_10 ${FTAIL} || \
                        error "join ${F75}_join_10 ${FTAIL} error"
                $CHECKSTAT -a ${FTAIL} || error "tail file exist after join"
        done
        cmp ${F75}_join_10 ${F75}_join_10_compare || \
                error "files ${F75}_join_10 ${F75}_join_10_compare differ"
}
run_test 75f "TEST join file 6 (join 10 files) =================="
 
test_75g() {
        [ ! -f ${F75}_join_10 ] && echo "${F75}_join_10 missing" && return
        $LFS getstripe ${F75}_join_10
 
        $OPENUNLINK ${F75}_join_10 ${F75}_join_10 || error "files unlink open"
 
        ls -l $F75*
}
run_test 75g "TEST join file 7 (open unlink) ===================="

num_inodes() {
	awk '/lustre_inode_cache/ {print $2; exit}' /proc/slabinfo
}

test_76() { # bug 1443
	DETH=$(grep deathrow /proc/kallsyms /proc/ksyms 2> /dev/null | wc -l)
	[ $DETH -eq 0 ] && skip "No _iget." && return 0
	BEFORE_INODES=`num_inodes`
	echo "before inodes: $BEFORE_INODES"
	for i in `seq 1000`; do
		touch $DIR/$tfile
		rm -f $DIR/$tfile
	done
	AFTER_INODES=`num_inodes`
	echo "after inodes: $AFTER_INODES"
	[ $AFTER_INODES -gt $((BEFORE_INODES + 32)) ] && \
		error "inode slab grew from $BEFORE_INODES to $AFTER_INODES"
	true
}
run_test 76 "destroy duplicate inodes in client inode cache ===="

export ORIG_CSUM=""
set_checksums()
{
	[ "$ORIG_CSUM" ] || ORIG_CSUM=`cat $LPROC/osc/*/checksums | head -n1`
	for f in $LPROC/osc/*/checksums; do
		echo $1 >> $f
	done

	return 0
}

F77_TMP=$TMP/f77-temp
F77SZ=8
setup_f77() {
	dd if=/dev/urandom of=$F77_TMP bs=1M count=$F77SZ || \
		error "error writing to $F77_TMP"
}

test_77a() { # bug 10889
	[ ! -f $F77_TMP ] && setup_f77
	set_checksums 1
	dd if=$F77_TMP of=$DIR/$tfile bs=1M count=$F77SZ || error "dd error"
	set_checksums 0
}
run_test 77a "normal checksum read/write operation ============="

test_77b() { # bug 10889
	[ ! -f $F77_TMP ] && setup_f77
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	sysctl -w lustre.fail_loc=0x80000409
	set_checksums 1
	dd if=$F77_TMP of=$DIR/f77b bs=1M count=$F77SZ conv=sync || \
		error "dd error: $?"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77b "checksum error on client write ===================="

test_77c() { # bug 10889
	[ ! -f $DIR/f77b ] && skip "requires 77b - skipping" && return  
	cancel_lru_locks osc
	#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
	sysctl -w lustre.fail_loc=0x80000408
	set_checksums 1
	cmp $F77_TMP $DIR/f77b || error "file compare failed"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77c "checksum error on client read ==================="

test_77d() { # bug 10889
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	sysctl -w lustre.fail_loc=0x80000409
	set_checksums 1
	directio write $DIR/f77 0 $F77SZ $((1024 * 1024)) || \
		error "direct write: rc=$?"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77d "checksum error on OST direct write ==============="

test_77e() { # bug 10889
	[ ! -f $DIR/f77 ] && skip "requires 77d - skipping" && return  
	#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
	sysctl -w lustre.fail_loc=0x80000408
	set_checksums 1
	cancel_lru_locks osc
	directio read $DIR/f77 0 $F77SZ $((1024 * 1024)) || \
		error "direct read: rc=$?"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77e "checksum error on OST direct read ================"

test_77f() { # bug 10889
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	sysctl -w lustre.fail_loc=0x409
	set_checksums 1
	directio write $DIR/f77 0 $F77SZ $((1024 * 1024)) && \
		error "direct write succeeded"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77f "repeat checksum error on write (expect error) ===="

test_77g() { # bug 10889
	[ $(grep -c obdfilter $LPROC/devices) -eq 0 ] && \
		skip "remote OST" && return
	[ ! -f $F77_TMP ] && setup_f77
	#define OBD_FAIL_OST_CHECKSUM_RECEIVE       0x21a
	sysctl -w lustre.fail_loc=0x8000021a
	set_checksums 1
	dd if=$F77_TMP of=$DIR/f77g bs=1M count=$F77SZ || \
		error "write error: rc=$?"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77g "checksum error on OST write ======================"

test_77h() { # bug 10889
	[ $(grep -c obdfilter $LPROC/devices) -eq 0 ] && \
		skip "remote OST" && return
	[ ! -f $DIR/f77g ] && skip "requires 77g - skipping" && return  
	cancel_lru_locks osc
	#define OBD_FAIL_OST_CHECKSUM_SEND          0x21b
	sysctl -w lustre.fail_loc=0x8000021b
	set_checksums 1
	cmp $F77_TMP $DIR/f77g || error "file compare failed"
	sysctl -w lustre.fail_loc=0
	set_checksums 0
}
run_test 77h "checksum error on OST read ======================="

[ "$ORIG_CSUM" ] && set_checksums $ORIG_CSUM || true
rm -f $F77_TMP
unset F77_TMP

test_78() { # bug 10901
 	NSEQ=5
	F78SIZE=$(($(awk '/MemFree:/ { print $2 }' /proc/meminfo) / 1024))
	echo "MemFree: $F78SIZE, Max file size: $MAXFREE"
	MEMTOTAL=$(($(awk '/MemTotal:/ { print $2 }' /proc/meminfo) / 2048))
	echo "MemTotal: $((MEMTOTAL * 2))"
	[ $F78SIZE -gt $MEMTOTAL ] && F78SIZE=$MEMTOTAL
	[ $F78SIZE -gt 512 ] && F78SIZE=512
	[ $F78SIZE -gt $((MAXFREE / 1024)) ] && F78SIZE=$((MAXFREE / 1024))
	SMALLESTOST=`lfs df $DIR |grep OST | awk '{print $4}' |sort -n |head -1`
	echo "Smallest OST: $SMALLESTOST"
	[ $F78SIZE -gt $((SMALLESTOST * $OSTCOUNT / 1024)) ] && \
		F78SIZE=$((SMALLESTOST * $OSTCOUNT / 1024))
	echo "File size: $F78SIZE"
	$SETSTRIPE $DIR/$tfile -c -1 || error "setstripe failed"
 	for i in `seq 1 $NSEQ`
 	do
 		FSIZE=$(($F78SIZE / ($NSEQ - $i + 1)))
 		echo directIO rdwr round $i of $NSEQ
  	 	$DIRECTIO rdwr $DIR/$tfile 0 $FSIZE 1048576||error "rdwr failed"
  	done

	rm -f $DIR/$tfile
}
run_test 78 "handle large O_DIRECT writes correctly ============"

test_79() { # bug 12743
	[ $(grep -c obdfilter $LPROC/devices) -eq 0 ] &&
		skip "skipping test for remote OST" && return

	wait_delete_completed

        BKTOTAL=`awk 'BEGIN{total=0}; {total+=$1}; END{print total}' \
                 $LPROC/obdfilter/*/kbytestotal`
        BKFREE=`awk 'BEGIN{free=0}; {free+=$1}; END{print free}' \
                $LPROC/obdfilter/*/kbytesfree`
        BKAVAIL=`awk 'BEGIN{avail=0}; {avail+=$1}; END{print avail}' \
                 $LPROC/obdfilter/*/kbytesavail`
        STRING=`df -P $MOUNT | tail -n 1 | awk '{print $2","$3","$4}'`
        DFTOTAL=`echo $STRING | cut -d, -f1`
        DFUSED=`echo $STRING  | cut -d, -f2`
        DFAVAIL=`echo $STRING | cut -d, -f3`
        DFFREE=$(($DFTOTAL - $DFUSED))

        ALLOWANCE=$((64 * $OSTCOUNT))

        if [ $DFTOTAL -lt $(($BKTOTAL - $ALLOWANCE)) ] ||  
           [ $DFTOTAL -gt $(($BKTOTAL + $ALLOWANCE)) ] ; then
                error "df total($DFTOTAL) mismatch OST total($BKTOTAL)"
        fi
        if [ $DFFREE -lt $(($BKFREE - $ALLOWANCE)) ] || 
           [ $DFFREE -gt $(($BKFREE + $ALLOWANCE)) ] ; then
                error "df free($DFFREE) mismatch OST free($BKFREE)"
        fi
        if [ $DFAVAIL -lt $(($BKAVAIL - $ALLOWANCE)) ] || 
           [ $DFAVAIL -gt $(($BKAVAIL + $ALLOWANCE)) ] ; then
                error "df avail($DFAVAIL) mismatch OST avail($BKAVAIL)"
        fi
}
run_test 79 "df report consistency check ======================="

test_80() { # bug 10718
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1M
        sync; sleep 1; sync
        BEFORE=`date +%s`
        cancel_lru_locks OSC
        AFTER=`date +%s`
        DIFF=$((AFTER-BEFORE))
        if [ $DIFF -gt 1 ] ; then
                error "elapsed for 1M@1T = $DIFF"
        fi
        true
}
run_test 80 "Page eviction is equally fast at high offsets too  ===="

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
	mkdir -p $DIR/d99cvsroot || error "mkdir $DIR/d99cvsroot failed"
	chown $RUNAS_ID $DIR/d99cvsroot || error "chown $DIR/d99cvsroot failed"
	$RUNAS cvs -d $DIR/d99cvsroot init || error "cvs init failed"
}
run_test 99a "cvs init ========================================="

test_99b() {
	[ ! -d $DIR/d99cvsroot ] && test_99a
	$RUNAS [ ! -w /tmp ] && skip "/tmp has wrong w permission -- skipping" && return
	cd /etc/init.d || error "cd /etc/init.d failed"
	# some versions of cvs import exit(1) when asked to import links or
	# files they can't read.  ignore those files.
	TOIGNORE=$(find . -type l -printf '-I %f\n' -o \
			! -perm +4 -printf '-I %f\n')
	$RUNAS cvs -d $DIR/d99cvsroot import -m "nomesg" $TOIGNORE \
		d99reposname vtag rtag > /dev/null || error "cvs import failed"
}
run_test 99b "cvs import ======================================="

test_99c() {
	[ ! -d $DIR/d99cvsroot ] && test_99b
	cd $DIR || error "cd $DIR failed"
	mkdir -p $DIR/d99reposname || error "mkdir $DIR/d99reposname failed"
	chown $RUNAS_ID $DIR/d99reposname || \
		error "chown $DIR/d99reposname failed"
	$RUNAS cvs -d $DIR/d99cvsroot co d99reposname > /dev/null || \
		error "cvs co d99reposname failed"
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
	netstat -tna | while read PROT SND RCV LOCAL REMOTE STAT; do
		[ "$PROT" != "tcp" ] && continue
		RPORT=`echo $REMOTE | cut -d: -f2`
		[ "$RPORT" != "$ACCEPTOR_PORT" ] && continue
		LPORT=`echo $LOCAL | cut -d: -f2`
		if [ $LPORT -ge 1024 ]; then
			echo "bad: $PROT $SND $RCV $LOCAL $REMOTE $STAT"
			netstat -tna
			error "local: $LPORT > 1024, remote: $RPORT"
		fi
	done
	true
}
run_test 100 "check local port using privileged port ==========="

function get_named_value()
{
    local tag

    tag=$1
    while read ;do
        line=$REPLY
        case $line in
        $tag*)
            echo $line | sed "s/^$tag//"
            break
            ;;
        esac
    done
}

export CACHE_MAX=`cat $LPROC/llite/*/max_cached_mb | head -n 1`
cleanup_101() {
	for s in $LPROC/llite/*/max_cached_mb; do
		echo $CACHE_MAX > $s
	done
	trap 0
}

test_101() {
	local s
	local discard
	local nreads=10000
	[ "$CPU" = "UML" ] && nreads=1000
	local cache_limit=32

	for s in $LPROC/osc/*/rpc_stats; do
		echo 0 > $s
	done
	trap cleanup_101 EXIT
	for s in $LPROC/llite/*; do
		echo 0 > $s/read_ahead_stats
		echo $cache_limit > $s/max_cached_mb
	done

	#
	# randomly read 10000 of 64K chunks from file 3x 32MB in size
	#
	echo "nreads: $nreads file size: $((cache_limit * 3))MB"
	$RANDOM_READS -f $DIR/$tfile -s$((cache_limit * 3192 * 1024)) -b65536 -C -n$nreads -t 180

	discard=0
	for s in $LPROC/llite/*; do
		discard=$(($discard + $(cat $s/read_ahead_stats | get_named_value 'read but discarded')))
	done
	cleanup_101

	if [ $(($discard * 10)) -gt $nreads ] ;then
		for s in $LPROC/osc/*/rpc_stats; do
			echo $s; cat $s
		done
		for s in $LPROC/llite/*/read_ahead_stats; do
			echo $s; cat $s
		done
		error "too many ($discard) discarded pages" 
	fi
	rm -f $DIR/$tfile || true
}
run_test 101 "check read-ahead for random reads ================"

export SETUP_TEST102=no
setup_test102() {
	[ "$SETUP_TEST102" = "yes" ] && return
	mkdir -p $DIR/$tdir
	STRIPE_SIZE=65536
	STRIPE_COUNT=4 
	STRIPE_OFFSET=2

	trap cleanup_test102 EXIT
	cd $DIR
	$SETSTRIPE $tdir -s $STRIPE_SIZE -i $STRIPE_OFFSET -c $STRIPE_COUNT
	cd $DIR/$tdir 
	for num in 1 2 3 4
	do
		for count in 1 2 3 4
		do
			for offset in 0 1 2 3 
			do
				local stripe_size=`expr $STRIPE_SIZE \* $num`
				local file=file"$num-$offset-$count"
				$SETSTRIPE $file -s $stripe_size -i $offset -c $count
			done
		done
	done

	cd $DIR
	star -c  f=$TMP/f102.tar $tdir 
	SETUP_TEST102=yes
}

cleanup_test102() {
	[ "$SETUP_TEST102" = "yes" ] || return
	trap 0
	rm -f $TMP/f102.tar
	rm -rf $DIR/$tdir
	SETUP_TEST102=no
}

test_102a() {
	local testfile=$DIR/xattr_testfile

	rm -f $testfile
        touch $testfile

	[ "$UID" != 0 ] && skip "must run as root" && return
	[ -z "`grep xattr $LPROC/mdc/*[mM][dD][cC]*/connect_flags`" ] && skip "must have user_xattr" && return
	[ -z "$(which setfattr 2>/dev/null)" ] && skip "could not find setfattr" && return

	echo "set/get xattr..."
        setfattr -n trusted.name1 -v value1 $testfile || error
        [ "`getfattr -n trusted.name1 $testfile 2> /dev/null | \
        grep "trusted.name1"`" == "trusted.name1=\"value1\"" ] || error
 
        setfattr -n user.author1 -v author1 $testfile || error
        [ "`getfattr -n user.author1 $testfile 2> /dev/null | \
        grep "user.author1"`" == "user.author1=\"author1\"" ] || error

	echo "listxattr..."
        setfattr -n trusted.name2 -v value2 $testfile || error
        setfattr -n trusted.name3 -v value3 $testfile || error
        [ `getfattr -d -m "^trusted" $testfile 2> /dev/null | \
        grep "trusted.name" | wc -l` -eq 3 ] || error

 
        setfattr -n user.author2 -v author2 $testfile || error
        setfattr -n user.author3 -v author3 $testfile || error
        [ `getfattr -d -m "^user" $testfile 2> /dev/null | \
        grep "user" | wc -l` -eq 3 ] || error

	echo "remove xattr..."
        setfattr -x trusted.name1 $testfile || error
        getfattr -d -m trusted $testfile 2> /dev/null | \
        grep "trusted.name1" && error || true

        setfattr -x user.author1 $testfile || error
        getfattr -d -m user $testfile 2> /dev/null | \
        grep "user.author1" && error || true

	# b10667: setting lustre special xattr be silently discarded
	echo "set lustre special xattr ..."
	setfattr -n "trusted.lov" -v "invalid value" $testfile || error

	rm -f $testfile
}
run_test 102a "user xattr test =================================="

test_102b() {
	# b10930: get/set/list trusted.lov xattr
	echo "get/set/list trusted.lov xattr ..."
	[ "$OSTCOUNT" -lt "2" ] && skip "skipping 2-stripe test" && return
	local testfile=$DIR/$tfile
	$SETSTRIPE $testfile -s 65536 -i 1 -c 2
	getfattr -d -m "^trusted" $testfile 2> /dev/null | \
	grep "trusted.lov" || error "can't get trusted.lov from $testfile"

	local testfile2=${testfile}2
	local value=`getfattr -n trusted.lov $testfile 2> /dev/null | \
		     grep "trusted.lov" |sed -e 's/[^=]\+=//'`
	
	$MCREATE $testfile2
	setfattr -n trusted.lov -v $value $testfile2 	
	local tmp_file=${testfile}3
	$GETSTRIPE -v $testfile2 > $tmp_file
	local stripe_size=`grep "size"  $tmp_file| awk '{print $2}'`
	local stripe_count=`grep "count"  $tmp_file| awk '{print $2}'`
	[ $stripe_size -eq 65536 ] || error "stripe size $stripe_size != 65536"
	[ $stripe_count -eq 2 ] || error "stripe count $stripe_count != 2"
}
run_test 102b "getfattr/setfattr for trusted.lov EAs ============"

test_102c() {
	# b10930: get/set/list lustre.lov xattr
	echo "get/set/list lustre.lov xattr ..."
	[ "$OSTCOUNT" -lt "2" ] && skip "skipping 2-stripe test" && return
	mkdir -p $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	local testfile=$DIR/$tdir/$tfile
	$RUNAS $SETSTRIPE $testfile -s 65536 -i 1 -c 2
	$RUNAS getfattr -d -m "^lustre" $testfile 2> /dev/null | \
	grep "lustre.lov" || error "can't get lustre.lov from $testfile"

	local testfile2=${testfile}2
	local value=`getfattr -n lustre.lov $testfile 2> /dev/null | \
		     grep "lustre.lov" |sed -e 's/[^=]\+=//'  `
	
	$RUNAS $MCREATE $testfile2
	$RUNAS setfattr -n lustre.lov -v $value $testfile2 	
	local tmp_file=${testfile}3
	$RUNAS $GETSTRIPE -v $testfile2 > $tmp_file
	local stripe_size=`grep "size"  $tmp_file| awk '{print $2}'`
	local stripe_count=`grep "count"  $tmp_file| awk '{print $2}'`
	[ "$stripe_size" -eq 65536 ] || error "stripe size $stripe_size != 65536"
	[ "$stripe_count" -eq 2 ] || error "stripe count $stripe_count != 2"
}
run_test 102c "non-root getfattr/setfattr for lustre.lov EAs ==========="

get_stripe_info() {
	stripe_size=0
	stripe_count=0
	stripe_offset=0
	local lines=`sed -n '/obdidx/=' $1`
	stripe_size=`awk '{if($1~/size/) print $2}' $1`
	stripe_count=`awk '{if($1~/count/) print $2}' $1`
	lines=`expr $lines + 1`
	stripe_offset=`sed -n ${lines}p $1 |awk '{print $1}'`
}

compare_stripe_info1() {
	for num in 1 2 3 4
	do
		for count in 1 2 3 4
		do
			for offset in 0 1 2 3 
			do
				local size=`expr $STRIPE_SIZE \* $num`
				local file=file"$num-$offset-$count"
				local tmp_file=out
				$GETSTRIPE -v $file > $tmp_file 
				get_stripe_info  $tmp_file
				if test $stripe_size -ne $size
				then
					error "$file: different stripe size" && return
				fi
				if test $stripe_count -ne $count
				then
					error "$file: different stripe count" && return
				fi
				if test $stripe_offset -ne 0
				then
					error "$file: different stripe offset" && return
				fi
				rm -f $tmp_file
			done
		done
	done
}

compare_stripe_info2() {
	for num in 1 2 3 4
	do
		for count in 1 2 3 4
		do
			for offset in 0 1 2 3 
			do
				local size=`expr $STRIPE_SIZE \* $num`
				local file=file"$num-$offset-$count"
				local tmp_file=out
				$GETSTRIPE -v $file > $tmp_file
				get_stripe_info  $tmp_file
				if test $stripe_size -ne $size
				then
					error "$file: different stripe size" && return	
				fi
				if test $stripe_count -ne $count
				then
					error "$file: different stripe count" && return
				fi
				if test $stripe_offset -ne $offset
				then
					error "$file: different stripe offset" && return
				fi
				rm -f $tmp_file
			done
		done
	done
}

test_102d() {
	# b10930: star test for trusted.lov xattr
	star --xhelp 2>&1 | grep -q nolustre  
	if [ $? -ne 0 ]
	then
		skip "being skipped because a lustre-aware star is not installed." && return
	fi
	[ "$OSTCOUNT" -lt "4" ] && skip "skipping 4-stripe test" && return
	setup_test102
	mkdir -p $DIR/d102d
	star -x  f=$TMP/f102.tar -C $DIR/d102d
	cd $DIR/d102d/$tdir
	compare_stripe_info1

}
run_test 102d "star restore stripe info from tarfile,not keep osts ==========="

test_102e() {
	# b10930: star test for trusted.lov xattr
	star --xhelp 2>&1 | grep -q nolustre  
	if [ $? -ne 0 ]
	then
		skip "being skipped because a lustre-aware star is not installed." && return
	fi
	[ "$OSTCOUNT" -lt "4" ] && skip "skipping 4-stripe test" && return
	setup_test102
	mkdir -p $DIR/d102e
	star -x  -preserve-osts f=$TMP/f102.tar -C $DIR/d102e
	cd $DIR/d102e/$tdir
	compare_stripe_info2
}
run_test 102e "star restore stripe info from tarfile, keep osts ==========="

test_102f() {
	# b10930: star test for trusted.lov xattr
	star --xhelp 2>&1 | grep -q nolustre  
	if [ $? -ne 0 ]
	then
		skip "being skipped because a lustre-aware star is not installed." && return
	fi
	[ "$OSTCOUNT" -lt "4" ] && skip "skipping 4-stripe test" && return
	setup_test102
	mkdir -p $DIR/d102f
	cd $DIR
	star -copy  $tdir $DIR/d102f
	cd $DIR/d102f/$tdir
	compare_stripe_info1
}
run_test 102f "star copy files, not keep osts ==========="

test_102g() {
	# b10930: star test for trusted.lov xattr
	star --xhelp 2>&1 | grep -q nolustre  
	if [ $? -ne 0 ]
	then
		skip "being skipped because a lustre-aware star is not installed." && return
	fi
	[ "$OSTCOUNT" -lt "4" ] && skip "skipping 4-stripe test" && return
	setup_test102
	mkdir -p $DIR/d102g
	cd $DIR
	star -copy -preserve-osts $tdir $DIR/d102g
	cd $DIR/d102g/$tdir
	compare_stripe_info2
	cleanup_test102
}
run_test 102g "star copy files, keep osts ==========="

run_acl_subtest()
{
    $LUSTRE/tests/acl/run $LUSTRE/tests/acl/$1.test
    return $?
}

test_103 () {
    [ "$UID" != 0 ] && skip "must run as root" && return
    [ -z "$(grep acl $LPROC/mdc/*[mM][dD][cC]*/connect_flags)" ] && skip "must have acl enabled" && return
    [ -z "$(which setfacl 2>/dev/null)" ] && skip "could not find setfacl" && return

    SAVE_UMASK=`umask`
    umask 0022
    cd $DIR

    echo "performing cp ..."
    run_acl_subtest cp || error
    echo "performing getfacl-noacl..."
    run_acl_subtest getfacl-noacl > /dev/null || error
    echo "performing misc..."
    run_acl_subtest misc > /dev/null || error
#    XXX add back permission test when we support supplementary groups.
#    echo "performing permissions..."
#    run_acl_subtest permissions || error
    echo "performing setfacl..."
    run_acl_subtest setfacl > /dev/null || error

    # inheritance test got from HP
    echo "performing inheritance..."
    cp $LUSTRE/tests/acl/make-tree . || error
    chmod +x make-tree || error
    run_acl_subtest inheritance > /dev/null || error
    rm -f make-tree

    cd $SAVE_PWD
    umask $SAVE_UMASK
}
run_test 103 "acl test ========================================="

test_104() {
	touch $DIR/$tfile
	lfs df || error "lfs df failed"
	lfs df -ih || error "lfs df -ih failed"
	lfs df -h $DIR || error "lfs df -h $DIR failed"
	lfs df -i $DIR || error "lfs df -i $DIR failed"
	lfs df $DIR/$tfile || error "lfs df $DIR/$tfile failed"
	lfs df -ih $DIR/$tfile || error "lfs df -ih $DIR/$tfile failed"
	
	OSC=`awk '/-osc-|OSC.*MNT/ {print $4}' $LPROC/devices | head -n 1`
	lctl --device %$OSC deactivate
	lfs df || error "lfs df with deactivated OSC failed"
	lctl --device %$OSC recover
	lfs df || error "lfs df with reactivated OSC failed"
}
run_test 104 "lfs df [-ih] [path] test ========================="

test_105a() {
	# doesn't work on 2.4 kernels
        touch $DIR/$tfile
        if [ -n "`mount | grep \"$DIR.*flock\" | grep -v noflock`" ];
        then
                flocks_test on -f $DIR/$tfile || error "fail flock on"
        else
                flocks_test off -f $DIR/$tfile || error "fail flock off"
        fi
}
run_test 105a "flock when mounted without -o flock test ========"

test_105b() {
        touch $DIR/$tfile
        if [ -n "`mount | grep \"$DIR.*flock\" | grep -v noflock`" ];
        then
                flocks_test on -c $DIR/$tfile || error "fail flock on"
        else
                flocks_test off -c $DIR/$tfile || error "fail flock off"
        fi
}
run_test 105b "fcntl when mounted without -o flock test ========"

test_105c() {
        touch $DIR/$tfile
        if [ -n "`mount | grep \"$DIR.*flock\" | grep -v noflock`" ];
        then
                flocks_test on -l $DIR/$tfile || error "fail flock on"
        else
                flocks_test off -l $DIR/$tfile || error "fail flock off"
        fi
}
run_test 105c "lockf when mounted without -o flock test ========"

test_106() { #bug 10921
	$DIR/$tdir && error "exec $DIR/$tdir succeeded"
	chmod 777 $DIR/$tdir || error "chmod $DIR/$tdir failed"
}
run_test 106 "attempt exec of dir followed by chown of that dir"

test_107() {
        CDIR=`pwd`
        cd $DIR
        ulimit -c unlimited
        sleep 60 &
        SLEEPPID=$!

        file=`cat /proc/sys/kernel/core_pattern`
        core_pid=`cat /proc/sys/kernel/core_uses_pid`
        [ $core_pid -eq 1 ] && file=$file.$SLEEPPID
        rm -f $file
        sleep 1

        kill -s 11 $SLEEPPID
        wait $SLEEPPID
        if [ -e $file ]; then
                size=`stat -c%s $file`
                [ $size -eq 0 ] && error "Zero length core file $file"
        else
                error "Fail to create core file $file"
        fi
        rm -f $file
        cd $CDIR
}
run_test 107 "Coredump on SIG"

test_115() {
	OSTIO_pre=$(ps -e|grep ll_ost_io|awk '{print $4}'|sort -n|tail -1|\
	    cut -c11-20)
        [ -z "$OSTIO_pre" ] && skip "no OSS threads" && \
	    return
        echo "Starting with $OSTIO_pre threads"

	NUMTEST=20000
	NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
	[ $NUMFREE -lt $NUMTEST ] && NUMTEST=$(($NUMFREE - 1000))
	echo "$NUMTEST creates/unlinks"
	mkdir -p $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile $NUMTEST
	unlinkmany $DIR/$tdir/$tfile $NUMTEST

	OSTIO_post=$(ps -e|grep ll_ost_io|awk '{print $4}'|sort -n|tail -1|\
	    cut -c11-20)

	# don't return an error
        [ $OSTIO_post -eq $OSTIO_pre ] && echo \
	    "FAIL: No addition ll_ost_io threads were created ($OSTIO_pre)" &&\
	    echo "This may be fine, depending on what ran before this test" &&\
	    echo "and how fast this system is." && return

        echo "Started with $OSTIO_pre threads, ended with $OSTIO_post"
}
run_test 115 "verify dynamic thread creation===================="

free_min_max () {
	wait_delete_completed
	AVAIL=($(cat $LPROC/osc/*[oO][sS][cC][-_]*/kbytesavail))
	echo OST kbytes available: ${AVAIL[@]}
	MAXI=0; MAXV=${AVAIL[0]}
	MINI=0; MINV=${AVAIL[0]}
	for ((i = 0; i < ${#AVAIL[@]}; i++)); do
	    #echo OST $i: ${AVAIL[i]}kb
	    if [ ${AVAIL[i]} -gt $MAXV ]; then
		MAXV=${AVAIL[i]}; MAXI=$i
	    fi
	    if [ ${AVAIL[i]} -lt $MINV ]; then
		MINV=${AVAIL[i]}; MINI=$i
	    fi
	done
	echo Min free space: OST $MINI: $MINV 
	echo Max free space: OST $MAXI: $MAXV 
}

test_116() {
	[ "$OSTCOUNT" -lt "2" ] && skip "too few OSTs" && return
	remote_mds && skip "remote MDS" && return

	echo -n "Free space priority "
	cat $LPROC/lov/*/qos_prio_free
       	DELAY=$(cat $LPROC/lov/*/qos_maxage | head -1 | awk '{print $1}')
	declare -a AVAIL
	free_min_max
	[ $MINV -gt 960000 ] && skip "too much free space in OST$MINI" &&\
		return

	# generate uneven OSTs
	mkdir -p $DIR/$tdir/OST${MINI}
	declare -i FILL
	FILL=$(($MINV / 4))
	echo "Filling 25% remaining space in OST${MINI} with ${FILL}Kb"
	$SETSTRIPE $DIR/$tdir/OST${MINI} -i $MINI -c 1
	i=1
	while [ $FILL -gt 0 ]; do
	    dd if=/dev/zero of=$DIR/$tdir/OST${MINI}/$tfile-$i bs=2M count=1 2>/dev/null
	    FILL=$(($FILL - 2048))
	    echo -n .
	    i=$(($i + 1))
	done
	FILL=$(($MINV / 4))
	sync
	sleep $DELAY

	free_min_max
	DIFF=$(($MAXV - $MINV))
	DIFF2=$(($DIFF * 100 / $MINV))
	echo -n "diff=${DIFF}=${DIFF2}% must be > 20% for QOS mode..."
	if [ $DIFF2 -gt 20 ]; then
	    echo "ok"
	else
	    echo "failed - QOS mode won't be used"
	    error "QOS imbalance criteria not met"
	    return
	fi

	MINI1=$MINI; MINV1=$MINV
	MAXI1=$MAXI; MAXV1=$MAXV

	# now fill using QOS
	echo writing a bunch of files to QOS-assigned OSTs
	$SETSTRIPE $DIR/$tdir -c 1
	i=1
	while [ $FILL -gt 0 ]; do
	    dd if=/dev/zero of=$DIR/$tdir/$tfile-$i bs=1024 count=200 2>/dev/null
	    FILL=$(($FILL - 200))
	    echo -n .
	    i=$(($i + 1))
	done
	echo "wrote $i 200k files"
	sync
	sleep $DELAY

	echo "Note: free space may not be updated, so measurements might be off"
	free_min_max
	DIFF2=$(($MAXV - $MINV))
	echo "free space delta: orig $DIFF final $DIFF2"
	[ $DIFF2 -gt $DIFF ] && echo "delta got worse!" 
	DIFF=$(($MINV1 - ${AVAIL[$MINI1]}))
	echo "Wrote $DIFF to smaller OST $MINI1"
	DIFF2=$(($MAXV1 - ${AVAIL[$MAXI1]}))
	echo "Wrote $DIFF2 to larger OST $MAXI1"
	[ $DIFF -gt 0 ] && echo "Wrote $(($DIFF2 * 100 / $DIFF - 100))% more data to larger OST $MAXI1"

	# Figure out which files were written where 
	UUID=$(awk '/'$MINI1': / {print $2; exit}' $LPROC/lov/${FSNAME}-clilov-*/target_obd)
	echo $UUID
        MINC=$($GETSTRIPE --obd $UUID $DIR/$tdir | wc -l)
	echo "$MINC files created on smaller OST $MINI1"
	UUID=$(awk '/'$MAXI1': / {print $2; exit}' $LPROC/lov/${FSNAME}-clilov-*/target_obd)
        MAXC=$($GETSTRIPE --obd $UUID $DIR/$tdir | wc -l)
	echo "$MAXC files created on larger OST $MAXI1"
	[ $MINC -gt 0 ] && echo "Wrote $(($MAXC * 100 / $MINC - 100))% more files to larger OST $MAXI1"
	[ $MAXC -gt $MINC ] || error "stripe QOS didn't balance free space"
}
run_test 116 "stripe QOS: free space balance ==================="

test_117() # bug 10891
{
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
        #define OBD_FAIL_OST_SETATTR_CREDITS 0x21e
        sysctl -w lustre.fail_loc=0x21e
        > $DIR/$tfile || error "truncate failed"
        sysctl -w lustre.fail_loc=0
        echo "Truncate succeeded."
}
run_test 117 "verify fsfilt_extend ============================="

# Reset async IO behavior after error case
reset_async() {
	FILE=$DIR/reset_async

	# Ensure all OSCs are cleared
	$LSTRIPE $FILE 0 -1 -1
        dd if=/dev/zero of=$FILE bs=64k count=$OSTCOUNT
	sync
        rm $FILE
}

test_118a() #bug 11710
{
	reset_async
	
 	multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
	DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
		return 1;
        fi
}
run_test 118a "verify O_SYNC works =========="

test_118b()
{
	reset_async

	#define OBD_FAIL_OST_ENOENT 0x217
	do_facet ost sysctl -w lustre.fail_loc=0x217
	multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
	RC=$?
	do_facet ost sysctl -w lustre.fail_loc=0
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)

	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
		return 1;
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
		return 1;
	fi

	echo "Dirty pages not leaked on ENOENT"

	# Due to the above error the OSC will issue all RPCs syncronously
	# until a subsequent RPC completes successfully without error.
	multiop $DIR/$tfile Ow4096yc
	rm -f $DIR/$tfile
	
	return 0
}
run_test 118b "Reclaim dirty pages on fatal error =========="

test_118c()
{
	reset_async

	#define OBD_FAIL_OST_EROFS               0x216
	do_facet ost sysctl -w lustre.fail_loc=0x216

	# multiop should block due to fsync until pages are written
	multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &
	MULTIPID=$!
	sleep 1

	if [[ `ps h -o comm -p $MULTIPID` != "multiop" ]]; then
		error "Multiop failed to block on fsync, pid=$MULTIPID"
	fi

        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $WRITEBACK -eq 0 ]]; then
		error "No page in writeback, writeback=$WRITEBACK"
	fi

	do_facet ost sysctl -w lustre.fail_loc=0
        wait $MULTIPID
	RC=$?
	if [[ $RC -ne 0 ]]; then
		error "Multiop fsync failed, rc=$RC"
	fi

        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi
	
	rm -f $DIR/$tfile
	echo "Dirty pages flushed via fsync on EROFS"
	return 0
}
run_test 118c "Fsync blocks on EROFS until dirty pages are flushed =========="

test_118d()
{
	reset_async

	#define OBD_FAIL_OST_BRW_PAUSE_BULK
	do_facet ost sysctl -w lustre.fail_loc=0x214
	# multiop should block due to fsync until pages are written
	multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &	
	MULTIPID=$!
	sleep 1

	if [[ `ps h -o comm -p $MULTIPID` != "multiop" ]]; then
		error "Multiop failed to block on fsync, pid=$MULTIPID"
	fi

        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $WRITEBACK -eq 0 ]]; then
		error "No page in writeback, writeback=$WRITEBACK"
	fi

        wait $MULTIPID || error "Multiop fsync failed, rc=$?"
	do_facet ost sysctl -w lustre.fail_loc=0

        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)	
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "Dirty pages gaurenteed flushed via fsync"
	return 0
}
run_test 118d "Fsync validation inject a delay of the bulk =========="

test_118f() {
        reset_async

        #define OBD_FAIL_OSC_BRW_PREP_REQ2        0x40a
        sysctl -w lustre.fail_loc=0x8000040a

	# Should simulate EINVAL error which is fatal
        multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi
	
        sysctl -w lustre.fail_loc=0x0
        
        LOCKED=$(grep -c locked $LPROC/llite/*/dump_page_cache)
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

        reset_async
	return 0
}
run_test 118f "Simulate unrecoverable OSC side error =========="

test_118g() {
        reset_async

	#define OBD_FAIL_OSC_BRW_PREP_REQ        0x406
        sysctl -w lustre.fail_loc=0x406

	# simulate local -ENOMEM
        multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
	
        sysctl -w lustre.fail_loc=0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        LOCKED=$(grep -c locked $LPROC/llite/*/dump_page_cache)
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi
	
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

        reset_async
	return 0
}
run_test 118g "Don't stay in wait if we got local -ENOMEM  =========="

test_118h() {
        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
        do_facet ost sysctl -w lustre.fail_loc=0x20e
	# Should simulate ENOMEM error which is recoverable and should be handled by timeout
        multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
	
        do_facet ost sysctl -w lustre.fail_loc=0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        LOCKED=$(grep -c locked $LPROC/llite/*/dump_page_cache)
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi
	
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

	return 0
}
run_test 118h "Verify timeout in handling recoverables errors  =========="

test_118i() {
        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
        do_facet ost sysctl -w lustre.fail_loc=0x20e
	
	# Should simulate ENOMEM error which is recoverable and should be handled by timeout
        multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &
	PID=$!
	sleep 5
	do_facet ost sysctl -w lustre.fail_loc=0
	
	wait $PID
        RC=$?
	if [[ $RC -ne 0 ]]; then
		error "got error, but should be not, rc=$RC"
	fi

        LOCKED=$(grep -c locked $LPROC/llite/*/dump_page_cache)
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi
	
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

	return 0
}
run_test 118i "Fix error before timeout in recoverable error  =========="

test_118j() {
        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK2     0x220
        do_facet ost sysctl -w lustre.fail_loc=0x220

	# return -EIO from OST
        multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
        do_facet ost sysctl -w lustre.fail_loc=0x0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        LOCKED=$(grep -c locked $LPROC/llite/*/dump_page_cache)
        DIRTY=$(grep -c dirty $LPROC/llite/*/dump_page_cache)
        WRITEBACK=$(grep -c writeback $LPROC/llite/*/dump_page_cache)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi
	
	# in recoverable error on OST we want resend and stay until it finished
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

 	return 0
}
run_test 118j "Simulate unrecoverable OST side error =========="

test_118k()
{
	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
	do_facet ost sysctl -w lustre.fail_loc=0x20e
	mkdir -p $DIR/$tdir

        for ((i=0;i<10;i++)); do
                (dd if=/dev/zero of=$DIR/$tdir/$tfile-$i bs=1M count=10 || \
			error "dd to $DIR/$tdir/$tfile-$i failed" )&
	        SLEEPPID=$!
                sleep 0.500s
	        kill $SLEEPPID
	        wait $SLEEPPID
        done

        sysctl -w lustre.fail_loc=0
}
run_test 118k "bio alloc -ENOMEM and IO TERM handling ========="

test_119a() # bug 11737
{
        BSIZE=$((512 * 1024))
        directio write $DIR/$tfile 0 1 $BSIZE
        # We ask to read two blocks, which is more than a file size.
        # directio will indicate an error when requested and actual
        # sizes aren't equeal (a normal situation in this case) and
        # print actual read amount.
        NOB=`directio read $DIR/$tfile 0 2 $BSIZE | awk '/error/ {print $6}'`
        if [ "$NOB" != "$BSIZE" ]; then
                error "read $NOB bytes instead of $BSIZE"
        fi
        rm -f $DIR/$tfile
}
run_test 119a "Short directIO read must return actual read amount"

test_119b() # bug 11737
{
        [ "$OSTCOUNT" -lt "2" ] && skip "skipping 2-stripe test" && return

        $SETSTRIPE $DIR/$tfile -c 2
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1 || error "dd failed"
        sync
        multiop $DIR/$tfile oO_RDONLY:O_DIRECT:r$((2048 * 1024)) || \
                error "direct read failed"
        rm -f $DIR/$tfile
}
run_test 119b "Sparse directIO read must return actual read amount"

test_119c() # bug 13099
{
        BSIZE=1048576
        directio write $DIR/$tfile 3 1 $BSIZE || error "direct write failed"
        directio readhole $DIR/$tfile 0 2 $BSIZE || error "reading hole failed"
        rm -f $DIR/$tfile
}
run_test 119c "Testing for direct read hitting hole"

LDLM_POOL_CTL_RECALC=1
LDLM_POOL_CTL_SHRINK=2

disable_pool_recalc() {
        for NSD in $LPROC/ldlm/namespaces/*$1*; do
                if test -f $NSD/pool/control; then
                        CONTROL=`cat $NSD/pool/control`
                        CONTROL=$((CONTROL & ~LDLM_POOL_CTL_RECALC))
                        echo "$CONTROL" > $NSD/pool/control
                fi
        done
}

enable_pool_recalc() {
        for NSD in $LPROC/ldlm/namespaces/*$1*; do
                if test -f $NSD/pool/control; then
                        CONTROL=`cat $NSD/pool/control`
                        CONTROL=$((CONTROL | LDLM_POOL_CTL_RECALC))
                        echo "$CONTROL" > $NSD/pool/control
                fi
        done
}

disable_pool_shrink() {
        for NSD in $LPROC/ldlm/namespaces/*$1*; do
                if test -f $NSD/pool/control; then
                        CONTROL=`cat $NSD/pool/control`
                        CONTROL=$((CONTROL & ~LDLM_POOL_CTL_SHRINK))
                        echo "$CONTROL" > $NSD/pool/control
                fi
        done
}

enable_pool_shrink() {
        for NSD in $LPROC/ldlm/namespaces/*$1*; do
                if test -f $NSD/pool/control; then
                        CONTROL=`cat $NSD/pool/control`
                        CONTROL=$((CONTROL | LDLM_POOL_CTL_SHRINK))
                        echo "$CONTROL" > $NSD/pool/control
                fi
        done
}

disable_pool() {
        disable_pool_shrink $1
        disable_pool_recalc $1
}

enable_pool() {
        enable_pool_shrink $1
        enable_pool_recalc $1
}

lru_resize_enable()
{
        enable_pool osc
        enable_pool "filter-$FSNAME"
        enable_pool mdc
        enable_pool "mds-$FSNAME"
}

lru_resize_disable()
{
        disable_pool osc
        disable_pool "filter-$FSNAME"
        disable_pool mdc
        disable_pool "mds-$FSNAME"
}

test_120a() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        mkdir -p $DIR/$tdir/d1
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120a "Early Lock Cancel: mkdir test ==================="

test_120b() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        touch $DIR/$tdir/f1
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120b "Early Lock Cancel: create test =================="

test_120c() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        mkdir -p $DIR/$tdir/d1 $DIR/$tdir/d2
        touch $DIR/$tdir/d1/f1
        cancel_lru_locks mdc
        stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        ln $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120c "Early Lock Cancel: link test ===================="

test_120d() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        touch $DIR/$tdir
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        chmod a+x $DIR/$tdir
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120d "Early Lock Cancel: setattr test ================="

test_120e() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        dd if=/dev/zero of=$DIR/$tdir/f1 count=1
        cancel_lru_locks mdc
        cancel_lru_locks osc
        dd if=$DIR/$tdir/f1 of=/dev/null
        stat $DIR/$tdir $DIR/$tdir/f1 > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        unlink $DIR/$tdir/f1
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120e "Early Lock Cancel: unlink test =================="

test_120f() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        mkdir -p $DIR/$tdir/d1 $DIR/$tdir/d2
        dd if=/dev/zero of=$DIR/$tdir/d1/f1 count=1
        dd if=/dev/zero of=$DIR/$tdir/d2/f2 count=1
        cancel_lru_locks mdc
        cancel_lru_locks osc
        dd if=$DIR/$tdir/d1/f1 of=/dev/null
        dd if=$DIR/$tdir/d2/f2 of=/dev/null
        stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2 > /dev/null
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        mv $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable
}
run_test 120f "Early Lock Cancel: rename test =================="

test_120g() {
        [ -z "`grep early_lock_cancel $LPROC/mdc/*/connect_flags`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable
        count=10000
        echo create $count files
        mkdir -p $DIR/$tdir
        cancel_lru_locks mdc
        cancel_lru_locks osc
        t0=`date +%s`
        
        can0=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk0=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        createmany -o $DIR/$tdir/f $count
        sync
        can1=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk1=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        t1=`date +%s`
        echo total: $((can1-can0)) cancels, $((blk1-blk0)) blockings
        echo rm $count files
        rm -r $DIR/$tdir
        sync
        can2=`awk '/ldlm_cancel/ {print $2}' $LPROC/ldlm/services/ldlm_canceld/stats`
        blk2=`awk '/ldlm_bl_callback/ {print $2}' $LPROC/ldlm/services/ldlm_cbd/stats`
        t2=`date +%s`
        echo total: $count removes in $((t2-t1))
        echo total: $((can2-can1)) cancels, $((blk2-blk1)) blockings
        sleep 2
        # wait for commitment of removal
        lru_resize_enable
}
run_test 120g "Early Lock Cancel: performance test ============="

test_121() { #bug 10589
	writes=`dd if=/dev/zero of=$DIR/$tfile count=1 2>&1 | awk 'BEGIN { FS="+" } /out/ {print $1}'`
	sysctl -w lustre.fail_loc=0x310
	cancel_lru_locks osc > /dev/null
	reads=`dd if=$DIR/$tfile of=/dev/null 2>&1 | awk 'BEGIN { FS="+" } /in/ {print $1}'`
	sysctl -w lustre.fail_loc=0
	[ "$reads" -eq "$writes" ] || error "read" $reads "blocks, must be" $writes
}
run_test 121 "read cancel race ================================="

test_122() { #bug 11544
        #define OBD_FAIL_PTLRPC_CLIENT_BULK_CB   0x508
        sysctl -w lustre.fail_loc=0x508
        dd if=/dev/zero of=$DIR/$tfile count=1
        sync
        sysctl -w lustre.fail_loc=0
}
run_test 122 "fail client bulk callback (shouldn't LBUG) ======="

test_123() # statahead(bug 11401)
{
        if [ -z "$(grep "processor.*: 1" /proc/cpuinfo)" ]; then
                log "testing on UP system. Performance may be not as good as expected."
        fi

        mkdir -p $DIR/$tdir

        for ((i=1, j=0; i<=100000; j=$i, i=$((i * 10)) )); do
                createmany -o $DIR/$tdir/$tfile $j $((i - j))

                grep '[0-9]' $LPROC/llite/*/statahead_max
                cancel_lru_locks osc
                stime=`date +%s`
                ls -l $DIR/$tdir > /dev/null
                etime=`date +%s`
                delta_sa=$((etime - stime))
                log "ls $i files with statahead:    $delta_sa sec"

                for client in $LPROC/llite/*; do
                        max=`cat $client/statahead_max`
                        cat $client/statahead_stats
                        echo 0 > $client/statahead_max
                done

                grep '[0-9]' $LPROC/llite/*/statahead_max
                cancel_lru_locks osc
                stime=`date +%s`
                ls -l $DIR/$tdir > /dev/null
                etime=`date +%s`
                delta=$((etime - stime))
                log "ls $i files without statahead: $delta sec"

                for client in /proc/fs/lustre/llite/*; do
                        cat $client/statahead_stats
                        echo $max > $client/statahead_max
                done

                if [ $delta_sa -gt $delta ]; then
                        log "ls $i files is slower with statahead!"
                fi

                [ $delta -gt 20 ] && break
                [ "$SLOW" = "no" -a $delta -gt 3 ] && break		
        done
        log "ls done"

        stime=`date +%s`
        rm -r $DIR/$tdir
        sync
        etime=`date +%s`
        delta=$((etime - stime))
        log "rm -r $DIR/$tdir/: $delta seconds"
        log "rm done"
        cat $LPROC/llite/*/statahead_stats
        # wait for commitment of removal
        sleep 2
}
run_test 123 "verify statahead work"

test_124a() {
	[ -z "`grep lru_resize $LPROC/mdc/*/connect_flags`" ] && \
               skip "no lru resize on server" && return 0
        cancel_lru_locks mdc
        lru_resize_enable
        NSDIR=`find $LPROC/ldlm/namespaces | grep mdc | head -1`

        # we want to test main pool functionality, that is cancel based on SLV
        # this is why shrinkers are disabled
        disable_pool_shrink "mds-$FSNAME"
        disable_pool_shrink mdc

        NR=2000
        mkdir -p $DIR/$tdir || error "failed to create $DIR/$tdir"

        LRU_SIZE=`cat $NSDIR/lru_size`

        # use touch to produce $NR new locks
        log "create $NR files at $DIR/$tdir"
        for ((i=0;i<$NR;i++)); do touch $DIR/$tdir/f$i; done

        LRU_SIZE_B=`cat $NSDIR/lru_size`
        if test $LRU_SIZE -ge $LRU_SIZE_B; then
                skip "No cached locks created!"
                cat $NSDIR/pool/state
                return 0
        fi
        LRU_SIZE_B=$((LRU_SIZE_B-LRU_SIZE))
        log "created $LRU_SIZE_B lock(s)"

        # we want to sleep 30s to not make test too long
        SLEEP=30
        SLEEP_ADD=2

        # we know that lru resize allows one client to hold $LIMIT locks for 10h
        MAX_HRS=10

        # get the pool limit
        LIMIT=`cat $NSDIR/pool/limit`

        # calculate lock volume factor taking into account data set size and the
        # rule that number of locks will be getting smaller durring sleep interval
        # and we need to additionally enforce LVF to take this into account.
        # Use $LRU_SIZE_B here to take into account real number of locks created
        # in the case of CMD, LRU_SIZE_B != $NR in most of cases
        LVF=$(($MAX_HRS * 60 * 60 * $LIMIT / $SLEEP))
        log "make client drop locks $LVF times faster so that ${SLEEP}s is enough to cancel $LRU_SIZE_B lock(s)"
        OLD_LVF=`cat $NSDIR/pool/lock_volume_factor`
        echo "$LVF" > $NSDIR/pool/lock_volume_factor
        log "sleep for $((SLEEP+SLEEP_ADD))s"
        sleep $((SLEEP+SLEEP_ADD))
        echo "$OLD_LVF" > $NSDIR/pool/lock_volume_factor
        LRU_SIZE_A=`cat $NSDIR/lru_size`

        [ $LRU_SIZE_B -gt $LRU_SIZE_A ] || {
                error "No locks dropped in "$((SLEEP+SLEEP_ADD))"s. LRU size: $LRU_SIZE_A"
                lru_resize_enable
                unlinkmany $DIR/$tdir/f $NR
                return
        }

        log "Dropped "$((LRU_SIZE_B-LRU_SIZE_A))" locks in "$((SLEEP+SLEEP_ADD))"s"
        lru_resize_enable
        log "unlink $NR files at $DIR/$tdir"
        unlinkmany $DIR/$tdir/f $NR
}
run_test 124a "lru resize ======================================="

test_124b() {
	[ -z "`grep lru_resize $LPROC/mdc/*/connect_flags`" ] && \
               skip "no lru resize on server" && return 0

        NSDIR=`find $LPROC/ldlm/namespaces | grep mdc | head -1`
        LIMIT=`cat $NSDIR/pool/limit`

	#define LDLM_DEFAULT_LRU_SIZE (100 * num_online_cpus())
        NR_CPU=$(awk '/processor/' /proc/cpuinfo | wc -l)
	test $NR_CPU -gt 1 && SUFFIX="(s)" || SUFFIX=""
	# 100 locks here is default value for non-shrinkable lru as well
        # as the order to switch to static lru managing policy
        LDLM_DEFAULT_LRU_SIZE=$((100 * NR_CPU))
	log "$NR_CPU CPU${SUFFIX} detected, LDLM_DEFAULT_LRU_SIZE = $LDLM_DEFAULT_LRU_SIZE"

        log "disable lru resize for $(basename $NSDIR)"
        echo $LDLM_DEFAULT_LRU_SIZE > $NSDIR/lru_size

        NR=$((LIMIT-(LIMIT/3)))
        mkdir -p $DIR/$tdir/disable_lru_resize || 
		error "failed to create $DIR/$tdir/disable_lru_resize"

        createmany -o $DIR/$tdir/disable_lru_resize/f $NR
        log "doing ls -la $DIR/$tdir/disable_lru_resize 3 times"
        stime=`date +%s`
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null
        etime=`date +%s`
        nolruresize_delta=$((etime-stime))
        log "ls -la time: $nolruresize_delta seconds"
        log "lru_size = $(cat $NSDIR/lru_size)"

        mkdir -p $DIR/$tdir/enable_lru_resize || 
		error "failed to create $DIR/$tdir/enable_lru_resize"
        
	# 0 locks means here flush lru and switch to lru resize policy 
        log "enable lru resize for $(basename $NSDIR)"
        echo 0 > $NSDIR/lru_size

        createmany -o $DIR/$tdir/enable_lru_resize/f $NR
        log "doing ls -la $DIR/$tdir/enable_lru_resize 3 times"
        stime=`date +%s`
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null
        etime=`date +%s`
        lruresize_delta=$((etime-stime))
        log "ls -la time: $lruresize_delta seconds"
        log "lru_size = $(cat $NSDIR/lru_size)"

        if test $lruresize_delta -gt $nolruresize_delta; then
                log "ls -la is $((lruresize_delta - $nolruresize_delta))s slower with lru resize enabled"
        elif test $nolruresize_delta -gt $lruresize_delta; then
                log "ls -la is $((nolruresize_delta - $lruresize_delta))s faster with lru resize enabled"
        else
                log "lru resize performs the same with no lru resize"
        fi
}
run_test 124b "lru resize (performance test) ======================="

test_125() { # 13358
	[ -z "$(grep acl $LPROC/mdc/*-mdc-*/connect_flags)" ] && skip "must have acl enabled" && return
	mkdir -p $DIR/d125 || error "mkdir failed"
	$SETSTRIPE $DIR/d125 -s 65536 -c -1 || error "setstripe failed"
	setfacl -R -m u:bin:rwx $DIR/d125 || error "setfacl $DIR/d125 failes"
	ls -ld $DIR/d125 || error "cannot access $DIR/d125"
}
run_test 125 "don't return EPROTO when a dir has a non-default striping and ACLs"

test_126() { # bug 12829/13455
	[ "$UID" != 0 ] && echo "skipping $TESTNAME (must run as root)" && return
	$RUNAS -u 0 -g 1 touch $DIR/$tfile || error "touch failed"
	gid=`ls -n $DIR/$tfile | awk '{print $4}'`
	rm -f $DIR/$tfile
	[ $gid -eq "1" ] || error "gid is set to" $gid "instead of 1"
}
run_test 126 "check that the fsgid provided by the client is taken into account"

TMPDIR=$OLDTMPDIR
TMP=$OLDTMP
HOME=$OLDHOME

log "cleanup: ======================================================"
check_and_cleanup_lustre
if [ "$I_MOUNTED" != "yes" ]; then
	sysctl -w lnet.debug="$OLDDEBUG" 2> /dev/null || true
fi

echo '=========================== finished ==============================='
[ -f "$SANITYLOG" ] && cat $SANITYLOG && grep -q FAIL $SANITYLOG && exit 1 || true
echo "$0: completed"
