#!/bin/sh

LCONF=${LCONF:-../utils/lconf}
NAME=${NAME:-local}

config=$NAME.xml
mkconfig=./$NAME.sh

if [ ! -f $config -o $mkconfig -nt $config ]; then
   $mkconfig $config || exit 1
fi

${LCONF} --gdb $config || exit 2
