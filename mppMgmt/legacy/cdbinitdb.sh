#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbinitdb
#
#       Initialize the database directories for a CDB cluster.
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="[ PGROOT=<root_directory> ] [ PGPORT=<base_port> ] $(basename $0) \n        [ {--dbroot|-D} <root_directory> ] [ {--port|-p} <base_port> ] \n        [ {--dbname|-N} { dbname | cdb} ] \n        [ {--ndbs|-n} { #dbs | 1 } ] \n        [ {--help} ]"

echo -e "$(basename $0): Initialize the database directories for a CDB cluster."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command initializes the directory structure(s) for a CDB installation."
    echo    "You are able to specify the number of PG instances that you wish to initialize"
    echo    "by passing the --ndbs or -N option to the command line. By default, a single "
    echo    "PG installation with a single listener is created."

    echo -e "\nThe database directory(s) are created under the directory specified in \$PGROOT"
    echo    "or using the '-D' option. Directories of the form \$PGROOT/\$dbname\$N are"
    echo -e "created for all the required databases in the cluster. If the directory"
    echo -e "\$PGROOT does not exist, it is created by this script. If --dbname is not set,"
    echo -e "it defaults to \"cdb\"."


    echo -e "\n\$PGPORT or the '-p' option should specify the TCP port number to be used"
    echo    "by the listener. If \$PGPORT is not set and '-p' is not specified, the port"
    echo    "number defaults to 10000."

    echo -e "\nIf \$PGROOT is not set and '-D' is not specified, the root directory of the"
    echo    "CDB cluster databases defaults to \"\$HOME/cdb-data\"."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}

# Source our common functions
source cdbfuncs

# A private function that updates the configuration files of all of the segment
# databases at creation time.
function updateConf
{
    local max_connections
    local shared_buffers

    case $OSTYPE in

        # These systems only support Basic Regular Expressions (BREs)
        solaris|darwin*)
            local space=$(echo -en " \t\f\\\n\r\v")
            sed -e "s/^#port = 5432$/port = $PGPORT/" \
                -e "s/^[$space]*max_connections[$space]*=\{0,1\}[$space]*[0-9]\{1,\}/max_connections = 10/" \
                -e "s/^[$space]*shared_buffers[$space]*=\{0,1\}[$space]*[0-9]\{1,\}/shared_buffers = 32/" \
                 "$PGDATA"/postgresql.conf > "$PGDATA"/postgresql.conf.new
            ;;

        # Everything else is presumed to support Extended Regular Expressions (EREs)
        *)
            sed -r -e "s/^#port = 5432$/port = $PGPORT/" \
                   -e "s/^[[:space:]]*max_connections[[:space:]]*=?[[:space:]]*[[:digit:]]+/max_connections = 10/" \
                   -e "s/^[[:space:]]*shared_buffers[[:space:]]*=?[[:space:]]*[[:digit:]]+/shared_buffers = 32/" \
                    "$PGDATA"/postgresql.conf > "$PGDATA"/postgresql.conf.new
            ;;
    esac

    VERSION_CONTROL=off mv -f "$PGDATA"/postgresql.conf.new "$PGDATA"/postgresql.conf
}


# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#
# Pick out command line arguments:
#
#     -D | --dbroot     <root_directory>        $PGROOT
#     -p | --port       <port>                  $PGPORT
#     -P | --dbname     <dbname>
#     -N                <#dbs>
#
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Some useful variables
positional=0
options_ended=
dbroot=
port=
ndbs=
dbname=

# Ok, loop through the command line args
#
# TODO: For some reason, the -X form of command line parsing, does not handle
# errors very well. Needs fixing at some point.
while [ $# -gt 0 ]
do
    if [ ! $options_ended ]; then
        OPTIND=1
        OPTARG='--'
        while getopts :D:n:N:P: option
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
                p)
                   port="$OPTARG"
                   ;;
                n)
                   ndbs="$OPTARG"
                   ;;
                N)
                   dbname="$OPTARG"
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
            --ndbs)
                ndbs=${2:?Option '--ndbs' requires a value}
                shift 2
                ;;
            --ndbs=*)
                ndbs=${val:?Option '--ndbs' requires a value}
                shift 1
                ;;
            --dbname)
                dbname=${2:?Option '--dbname' requires a value}
                shift 2
                ;;
            --dbname=*)
                dbname=${val:?Option '--dbname' requires a value}
                shift 1
                ;;
            --port)
                port=${2:?Option '--port' requires a value}
                shift 2
                ;;
            --port=*)
                port=${val:?Option '--port' requires a value}
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

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Now that all the options and arguments have been parsed out, ensure
# that we have everthing that is required.
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Do we have a valid set of segments?
# @design (Todd) Not sure why there was a limit on 8 dbs previously. We can
# remove this if needed.
if [ "$ndbs" ]
then
    if expr "$ndbs" : '[1-9][0-9]*$' 1>/dev/null
    then
       :
    else
       echo "Number of instances must be specified as a number greater than zero" >&2
       exit 2
    fi
else
    # If nothing is specified, then default to 1
    : ${ndbs:=1}
fi

# Check what port was specified
if [ "$port" -o "$PGPORT" ]
then
    PGPORT=${port:-$PGPORT}
    if expr "$PGPORT" : '[1-9][0-9]*$' 1>/dev/null && [ 1024 -lt $PGPORT -a $PGPORT -le 65530 ]
    then
        :
    else
        echo "Port number must be from 1025 to 65530" >&2
        exit 2
    fi
else
    : ${PGPORT:=10000}
fi

# And what about the root dir?
PGROOT=${dbroot:-$PGROOT}
: ${PGROOT:=$HOME/cdb-data}

# And what about our dbname?
dbname=${dbname:-$dbname}
: ${dbname:=cdb}

if [ ! -d "$PGROOT" ]
then
    echo "Creating directory $PGROOT"
    mkdir -p "$PGROOT" || exit 3

fi


# ---------------------------------------------------------------------
# CDB database processing
# ---------------------------------------------------------------------
for (( i = 1; i <= ndbs; i++ ))
do
    PGDATA="$PGROOT/$dbname$i"

    echo -e "\nInitializing database at $PGDATA (port=$PGPORT)\n"

    if LC_ALL=C initdb --locale=C --encoding=UTF8 -D $PGDATA

    then
       updateConf
    else
       echo "initdb failed for database $i; terminating"
       exit 1
    fi

    # Increment our port number
    PGPORT=$(( PGPORT + 1 ))

done
