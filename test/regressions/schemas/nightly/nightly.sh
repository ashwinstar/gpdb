#!/bin/bash

# Initialize the environment
CDB_SCRIPTS=/opt/damagecontrol/work/cdb2/checkout/scripts
source $CDB_SCRIPTS/nightly/env.rc

echo "Initializing CDB2 database"

# Initialize the database
$CDB_SCRIPTS/cdbstop
$CDB_SCRIPTS/cdbclean
$CDB_SCRIPTS/cdbinitdb -n 3
$CDB_SCRIPTS/cdbstart
sleep 30
$CDB_SCRIPTS/cdbcreatedb -s cdbunit --withQadb

sleep 60
