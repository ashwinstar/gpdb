#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbsql
#
#       Connect psql to a segment database of a CDB cluster.
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="$(basename $0) [ {--utility|-u} ] [ {--help} ] [ <psql_options> ]"

echo -e "$(basename $0): Connect psql to a segment database of a CDB cluster."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command runs the psql command allowing you to connect to a segment database "
    echo    "in the CDB cluster. The CDB cluster must have been previously initialized"
    echo    "using \"cdbinitdb\" and started using \"cdbstart\". It is also recommended "
    echo -e "that a database was previously created using \"cdbcreatedb \"."

    echo -e "\nIf you wish to connect in utility mode, then use the --utility or -u"
    echo    "command line option. Any other command line options get passed directly"
    echo    "to psql."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}

# Some useful variables
utility_mode=false
options=

for (( i = 1; i <= $#; i++ ))
do
    case ${!i} in
        '-h' | --help)
            display_help
            ;;
        '-u' | --utility)
            utility_mode=true
            ;;
        *)
            # Default action, append the argument to our options string
            options=("$options ${!i}")
            ;;
    esac

done

# Should we be in utility mode?
if [ "$utility_mode" = "true" ]
then
  export PGOPTIONS="-c cdb_session_role=utility"
fi

# Run the command
psql $options
