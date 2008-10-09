#!/bin/bash

if [ x$1 != x ] ; then
    GPPERFMON_HOME=$1
else
    GPPERFMON_HOME="\`pwd\`"
fi

cat<<EOF
GPPERFMONHOME=${GPPERFMON_HOME}
PATH=${GPPERFMON_HOME}/bin:\$PATH

export GPPERFMONHOME
export PATH
EOF
