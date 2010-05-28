#!/bin/bash
## ======================================================================
## $Header$
## ======================================================================

cat <<-EOF
	
	======================================================================
	timestamp: $( date )
	----------------------------------------------------------------------
	
EOF

gpdiff=${BLDWRAP_TOP}/src/cdb-pg/src/test/regress/gpdiff.pl
ereportdir=${BLDWRAP_TOP}/src/releng/ereport

cd ${BLDWRAP_TOP}/src/releng/ereport
p4 edit ereport.txt
cp ereport.txt ereport-prev.txt

cd ${BLDWRAP_TOP}/src/cdb-pg/src
find . -name '*.c'  | xargs perl ${BLDWRAP_TOP}/src/releng/ereport/get_ereport.pl \
                                 -quiet \
                                 -lax \
                                 -fullname \
                                 > ${ereportdir}/ereport.txt

cd ${ereportdir}

${gpdiff} -gpd_init ereport-gpdiff-init.txt ereport-prev.txt ereport.txt > ereport-diff.txt
if [ $? -ne 0 ]; then
    p4 submit -d "Automated update of ereport.txt" ereport.txt
else
    echo "No need to submit ereport.txt"
    p4 revert ereport.txt
    rm -f ereport-diff.txt
fi

cat <<-EOF
	
	======================================================================
	
EOF
