#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbstop
#
#       Stops the listener(s) of a CDB cluster.
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="[ PGROOT=<root_directory> ] $(basename $0) \n        [ {--dbroot|-D} <root_directory> ] \n        [ {--prefix|-P} { dbprefix | cdb} ] \n        [ {--help} ]"

echo -e "$(basename $0): Stops all listeners of a CDB cluster."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command stops the amsd and listeners of a CDB cluster.The CDB cluster must"
    echo    "have been previously initialized using \"cdbinitdb\" and started using \"cdbstart\"."

    echo -e "\nIf \$PGROOT or --dbroot is not set, the root of the database installation "
    echo -e "defaults to \"\$HOME/cdb-data\". You can also specify which database(s) to stop "
    echo -e "using the --prefix option. Unless set, this defaults to \"cdb\"."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#
# Pick out command line arguments:
#
#     -D | --dbroot     <root_directory>        $PGROOT
#     -p | --prefix     <db prefix>
#     -v | --verbose    <verbose>
#
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Source our utility functions
source cdbfuncs

# Some useful variables
positional=0
options_ended=
dbroot=
prefix=

# Ok, loop through the command line args
#
# TODO: For some reason, the -X form of command line parsing, does not handle
# errors very well. Needs fixing at some point.
while [ $# -gt 0 ]
do
    if [ ! $options_ended ]; then
        OPTIND=1
        OPTARG='--'
        while getopts :D:v:P: option
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
                v)
                   verbose="true"
                   ;;
                *)
                   display_help
                   exit 1
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
                positional=$((positional + 1))
                case $positional in
                    *)
                        echo "Unrecognized argument(s) beginning with $1" >&2
                        exit 1
                        ;;
                esac
                shift 1
                ;;
        esac
    fi

done

# Make sure our variables are populated
: ${PGROOT:=$HOME/cdb-data}

# And what about our prefix?
prefix=${prefix:-$prefix}
: ${prefix:=cdb}

# Source our utility functions
source cdbfuncs

# Discard outside values of these
unset PGHOST
unset PGHOSTADDR

# The following vary depending on the target node
export PGDATA
export PGPORT


# ---------------------------------------------------------------------
# Stop all the DB instances
# ---------------------------------------------------------------------
shopt -s extglob
for PGDATA in "$PGROOT"/$prefix*[0-9]?([0-9])
do
    [ ! -d "$PGROOT" ] && continue

    db=$(basename "$PGDATA")
    PGPORT=$(getPort)

    echo "Stopping ($PGDATA on port $PGPORT)"
    pg_ctl stop
done

# ---------------------------------------------------------------------
# Stop the amsd
# ---------------------------------------------------------------------
echo "Shutting down amsd"
stopams

# Sometimes bugs leave ams-svc processes around!
killall ams-svc > /dev/null

# Stop lam
lamhalt
