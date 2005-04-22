#!/bin/bash

# Initialize the environment
CDB_SCRIPTS=/opt/damagecontrol/work/cdb2/checkout/scripts
source $CDB_SCRIPTS/nightly/env.rc

cd $CDB_SCRIPTS

echo "Initializing CDB2 database"

# Initialize the database
./cdbstop.sh
./cdbclean.sh
./cdbinitdb.sh -n 3
./cdbstart.sh
sleep 30
./cdbcreatedb.sh -s cdbunit --withQadb
