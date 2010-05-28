#!/bin/bash
## ======================================================================
### $Header$
## ======================================================================

WORKDIR=/Users/eespino/eng/all/cdb2/main/
ereportdir=${WORKDIR}/src/releng/ereport

cd ${WORKDIR}/src/releng/ereport
p4 edit ereport.txt

cd ${WORKDIR}/src/cdb-pg/src
find . -name '*.c'  | xargs perl ${WORKDIR}/src/releng/ereport/get_ereport.pl \
                                 -quiet \
                                 -lax \
                                 -fullname \
                                 > ${ereportdir}/ereport.txt

cd ${ereportdir}
p4 revert -a ereport.txt

p4 opened ereport.txt 2>&1 | grep "not opened" > /dev/null
if [ $? -ne 0 ]; then
    p4 submit -d "Automated update of ereport.txt" ereport.txt
fi
