#!/bin/bash

. ./lib/platform_defines.sh

#Tarfile to use
TARFILE=`ls | grep '^greenplum-perfmon-web.*.tar.gz'`
INSTALLER_NAME=`echo ${TARFILE} | sed 's,.tar.gz,,'`
TARFILE_SIZE=`ls -al ${TARFILE} | awk '{print $5}'`
BOM="installer-bom.txt"

#Version string
GPPERFMON_VERSION=$( perl -pe 's, ,-,g' ../../VERSION )

if [ x"${GPPERFMON_VERSION}" = "x" ] ; then
    echo "Error: GPPerfMon Version file not found"
    exit 1
fi

#Perform some checks
if [ -z "$TARFILE" ] ; then
    echo "Could not find GPPerfMon tarfile."
    exit 1
fi

echo "Creating installer"
echo "Tar file: ${TARFILE}" 
echo "Size: ${TARFILE_SIZE}b"
echo""

#Get this from a checked in file eventually...
./generate-gpperfmon-installer-header.sh "${GPPERFMON_VERSION}" | sed "s,PLACEHOLDER,${TARFILE}," \
    > ${INSTALLER_NAME}.bin
cat ${TARFILE} >> ${INSTALLER_NAME}.bin
chmod 755 ${INSTALLER_NAME}.bin

if [ "$?" -ne "0" ] ; then
    echo "Error creating installer."
    exit 1
fi

echo "${INSTALLER_NAME}.bin created."
