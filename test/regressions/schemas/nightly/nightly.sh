#!/bin/bash

# Initialize the environment
CDB_SCRIPTS=/opt/damagecontrol/work/cdb2/checkout/scripts
source $CDB_SCRIPTS/nightly/env.rc

cd $CDB_SCRIPTS

echo "Initializing CDB2 database"

# Initialize the database
./cdbstop
./cdbclean
./cdbinitdb -n 3
./cdbstart
sleep 30
./cdbcreatedb -s cdbunit --withQadb
