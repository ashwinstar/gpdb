#!/bin/bash
## ======================================================================
## $Header$
## ======================================================================

cat <<-EOF
	
	======================================================================
	timestamp: $( date )
	----------------------------------------------------------------------
	
EOF

ereportdir=${BLDWRAP_TOP}/src/releng/ereport

cd ${BLDWRAP_TOP}/src/releng/ereport
p4 edit ereport.txt

cd ${BLDWRAP_TOP}/src/cdb-pg/src
find . -name '*.c'  | xargs perl ${BLDWRAP_TOP}/src/releng/ereport/get_ereport.pl \
                                 -quiet \
                                 -lax \
                                 -fullname \
                                 > ${ereportdir}/ereport.txt

cd ${ereportdir}
p4 revert -a ereport.txt

p4 opened ereport.txt 2>&1 | grep "not opened" > /dev/null
if [ $? -ne 0 ]; then
    p4 diff ereport.txt > ereport-diff.txt
    p4 submit -d "Automated update of ereport.txt" ereport.txt
else
    echo "No need to submit ereport.txt"
fi

cat <<-EOF
	
	======================================================================
	
EOF
