#!/bin/sh -e 

CONFLICTS=cvs-merge-conflicts
CVS="cvs -z3"

if [ -f .mergeinfo ] ; then
    echo ".mergeinfo exists - clean up first"
    exit 
fi

if [ -f $CONFLICTS ] ; then
    echo "$CONFLICTS exists - clean up first"
    exit 
fi

if [ $# != 2 ]; then
    echo "This is phase 1 of merging branches. Usage: $0 parent child"
    exit
fi

parent=$1
PARENT=`echo $parent | sed -e "s/^b_//" | tr "[a-z]" "[A-Z]"`
child=$2
CHILD=`echo $child | sed -e "s/^b_//" | tr "[a-z]" "[A-Z]"`
date=`date +%Y%m%d_%H%M`
module=lustre

case $parent in
  HEAD) : ;;
  b_*|b[1-4]*) : ;;
  *) parent="b_$parent" ;;
esac
case $child in
  HEAD) : ;;
  b_*|b[1-4]*) : ;;
  *) child="b_$child"
esac

if [ "$child" != "HEAD" -a "`cat CVS/Tag 2> /dev/null`" != "T$child" ]; then
	echo "This script must be run within the $child branch"
	exit 1
fi

TEST_FILE=${TEST_FILE:-ChangeLog} # does this need to be smarter?
check_tag() {
	[ -z "$1" ] && echo "check_tag() missing arg" && exit3
	[ "$1" = "HEAD" ] && return
	$CVS log $TEST_FILE | grep -q "	$1: " && return
	echo "$0: tag $1 not found in $TEST_FILE"
	exit 2
}

check_tag $parent
check_tag ${CHILD}_BASE

cat << EOF > .mergeinfo
parent=$parent
PARENT=$PARENT
child=$child
CHILD=$CHILD
date=$date
module=$module
CONFLICTS=$CONFLICTS
OPERATION=Merge
OPERWHERE=from
EOF

echo PARENT: $PARENT parent: $parent CHILD: $CHILD child: $child date: $date

echo -n "tagging $parent as '${PARENT}_${CHILD}_UPDATE_PARENT_$date' ...."
$CVS rtag -r $parent ${PARENT}_${CHILD}_UPDATE_PARENT_$date $module
echo "done"
echo -n "tagging $child as '${PARENT}_${CHILD}_UPDATE_CHILD_$date' ...."
$CVS rtag -r $child ${PARENT}_${CHILD}_UPDATE_CHILD_$date $module
echo "done"

# Apply all of the changes to your local tree:
echo "Updating: -j ${CHILD}_BASE -j ${PARENT}_${CHILD}_UPDATE_PARENT_$date ...."
$CVS update -j ${CHILD}_BASE -j ${PARENT}_${CHILD}_UPDATE_PARENT_$date -dP
echo "done"

echo -n "Recording conflicts in $CONFLICTS ..."
if $CVS update | grep '^C' > $CONFLICTS; then
    echo "Conflicts found, fix before committing."
    cat $CONFLICTS
else 
    echo "No conflicts found"
    rm -f $CONFLICTS
fi
echo "done"

echo "Test, commit and then run merge2.sh (no arguments)"
