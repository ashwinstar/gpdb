#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbpsql
#
#       Connect psql to a segment database of a CDB cluster.
#
# TODO: This script actually doesn't handle passing options to psql very
# well at all. It cuts off the first part. Needs work!
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="PGROOT=<root_directory> $(basename $0) \n        [ {--dbroot|-D} <root_directory> ] \n        [ {--prefix|-P} { dbprefix | cdb} ] \n        [ {--help} ] \n        [ <psql_options> ]"

echo -e "$(basename $0): Connect psql to a segment database of a CDB cluster."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command runs the psql command connecting to a segment database "
    echo    "in the CDB cluster. The CDB cluster must have been previously initialized using"
    echo    "\"cdbinitdb\" and started using \"cdbstart\". It is also recommended "
    echo -e "that a database was previously created using \"cdbcreatedb \"."

    echo -e "\nIf \$PGROOT is not set, the root of the CDB cluster containing the "
    echo -e "target database defaults to \"\$HOME/cdb-data\". \$PGDATA is determined"
    echo -e "by appending \$dbprefix to \$PGROOT. If \$dbprefix is not set it "
    echo -e "defaults to \"cdb\"."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#
# Pick out command line arguments:
#
#     -D | --dbroot     <root_directory>        $PGROOT
#     -P | --prefix     <dbprefix>
#
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Some useful variables
positional=0
options_ended=
dbroot=
prefix=
options=

# Ok, loop through the command line args
#
# TODO: For some reason, the -X form of command line parsing, does not handle
# errors very well. Needs fixing at some point.
while [ $# -gt 0 ]
do
    if [ ! $options_ended ]; then
        OPTIND=1
        OPTARG='--'
        while getopts :D:P: option
        do
            case $option in
                :)
                   echo "$(basename $0): Option '$OPTARG' requires a value" >&2
                   exit 1
                   ;;
                \?)
                   # This case is hit for the first long option or argument.
                   [ "$OPTARG" != "?" ] && break
                   ;;
                D)
                   dbroot="$OPTARG"
                   ;;
                P)
                   prefix="$OPTARG"
                   ;;
            esac
            OPTARG='--'
        done

        #
        # If we get here with options consumed and OPTARG set to '--'
        # we've hit the "options end" token '--'.  Prepare NOT to
        # return through options processing.
        #
        if [ "$OPTARG" = "--" -a $OPTIND -gt 0 ]; then
            options_ended=true
        fi

        unset option
        shift $(($OPTIND - 1))
    fi

    #
    # Handle long options and positional parameters
    #
    if [ $# -gt 0 ]; then
        val="$(expr "$1" : '--[^=]*=\(.*\)')"
        case "$1" in
            --help)
                display_help
                exit 1
                ;;
            --dbroot)
                dbroot=${2:?Option '--dbroot' requires a value}
                shift 2
                ;;
            --dbroot=*)
                dbroot=${val:?Option '--dbroot' requires a value}
                shift 1
                ;;
            --prefix)
                prefix=${2:?Option '--prefix' requires a value}
                shift 2
                ;;
            --prefix=*)
                prefix=${val:?Option '--prefix' requires a value}
                shift 1
                ;;
            --*)
                echo "$(basename $0): Unrecognized option \"$1\"" >&2
                echo -e "\nSyntax: $usage"
                exit 1
                ;;
            *)
                options="$options $1"
                shift 1
                ;;
        esac
    fi

done


# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Now that all the options and arguments have been parsed out, ensure
# that we have everthing that is required.
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PGROOT=${dbroot:-$PGROOT}
: ${PGROOT:=$HOME/cdb-data}

# And what about our prefix?
prefix=${prefix:-$prefix}
: ${prefix:=cdb}

# And the options?
options=${options:-$options}
: ${options:=segDB1}

source cdbfuncs

# Discard outside values of these
unset PGHOST
unset PGHOSTADDR

# The following vary depending on the target node
export PGDATA
export PGPORT

# ---------------------------------------------------------------------
# Run psql connecting to the first DB we find
# ---------------------------------------------------------------------
shopt -s extglob

# TODO: Add some real error checking here.
# TODO: Also, doing this in a for loop sucks! I couldn't figure out how
# to get the assignment to work otherwise though. This is OK for now.
for PGDATA in "$PGROOT"/$prefix*;
do

    if [ -d "$PGDATA" ]
    then
        PGPORT=$(getPort)

        echo -e "\nConnecting to database $PGDATA on port $PGPORT\n"
        #echo "Options = $options"
        psql $options
        break;
    else
        echo -e "\nDid not find any database in $PGROOT with prefix \"$prefix\""
        exit 1
    fi
done
