#!/bin/bash
## ======================================================================
## ======================================================================

gpdiff=${BLDWRAP_TOP}/src/cdb-pg/src/test/regress/gpdiff.pl
ereportdir=${BLDWRAP_TOP}/src/releng/ereport

cd ${ereportdir}

cat <<-EOF
	
	======================================================================
	timestamp .. : $( date )
	cwd ........ : $( pwd )
	----------------------------------------------------------------------
	
EOF

##
## In case ereport is in a previously edited state, revert it.
##

p4 revert ereport.txt

p4 edit ereport.txt
if [ $? != 0 ]; then
    echo "ERROR: p4 edit failed for ereport.txt"
else
	cp ereport.txt ereport-prev.txt
	
	pushd ${BLDWRAP_TOP}/src/cdb-pg/src
	find . -name '*.c'  | xargs perl ${BLDWRAP_TOP}/src/releng/ereport/get_ereport.pl \
	                                 -quiet \
	                                 -lax \
	                                 -elog \
	                                 -fullname \
	                                 > ${ereportdir}/ereport.txt
	
	popd
	
	${gpdiff} -gpd_init ereport-gpdiff-init.txt ereport-prev.txt ereport.txt > ereport-diff.txt
	if [ $? -ne 0 ]; then
	    p4 submit -d "Automated update of ereport.txt" ereport.txt
	else
	    echo "No need to submit ereport.txt"
	    p4 revert ereport.txt
	    rm -f ereport-diff.txt
	fi
fi

cat <<-EOF
	
	======================================================================
	
EOF
