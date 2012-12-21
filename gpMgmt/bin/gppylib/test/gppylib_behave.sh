#!/bin/bash

source $HOME/qa.sh

cd $BLDWRAP_TOP/cdb2/gpMgmt/bin

make behave $1 2>&1 
