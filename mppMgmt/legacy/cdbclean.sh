#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbclean
#
#       Delete a cdb cluster installation.
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="[ PGROOT=<root_directory> ] $(basename $0) \n        [ {--dbroot|-D} <root_directory> ] \n        [ {--prefix|-P} { dbprefix | cdb*} ] \n        [ {--help} ]"

echo -e "cdbclean: Removes a cdb installation directory structure."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command will remove the database directories that were created using"
    echo -e "\"cdbinitdb\" Before doing this, you should have stopped cdb using \"cdbstop\"."

    echo -e "\nIf \$PGROOT is not set, or --dbroot or -D are not passed, the location "
    echo    "containing the data directories of the CDB cluster to delete defaults "
    echo -e "to \"\$HOME/cdb-data\". You can control which databases are deleted by "
    echo -e "passing the --prefix option on the command line. If it is not specified,"
    echo -e "it defaults \"cdb*\"."

    echo -e "\nNOTE: The directories to be deleted are matched using the '*' wildcard"
    echo -e "pattern. E.g. if you set --prefix=cdb and you have both cdb1 and cdb2 in "
    echo -e "\$PGROOT then they will both get deleted. The corresponding log files are"
    echo -e "also deleted."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}


# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#
# Pick out command line arguments:
#
#     -D | --dbroot     <root_directory>        $PGROOT
#     -P | --prefix     <dbname prefix>
#
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Some useful variables
positional=0
options_ended=
dbroot=
prefix=

# Loop through our command line args, and see what we have
# TODO: Figure out why we don't trap bad options in the first block
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

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Now that all the options and arguments have been parsed out, ensure
# that we have everthing that is required.
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

PGROOT=${dbroot:-$PGROOT}
: ${PGROOT:=$HOME/cdb-data}

# And what about our prefix?
prefix=${prefix:-$prefix}
: ${prefix:=cdb}


# ---------------------------------------------------------------------
# Remove the diretories
# ---------------------------------------------------------------------
shopt -s extglob

for PGDATA in "$PGROOT"/$prefix*
do
    [ ! -d "$PGDATA" ] && continue

    echo "Deleting DB instance $PGDATA"
    rm -rf $PGDATA

    echo "Deleting corresponding log file"
    fullprefix=$(basename "$PGDATA")
    rm -r $PGROOT/logfile.$fullprefix
done

echo "Deleting amsd.log"
rm -f $PGROOT/amsd.log
