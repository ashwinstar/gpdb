#!/bin/bash
# ---------------------------------------------------------------------
#
# cdbcreatedb
#
#       Create and load a database into a CDB cluster.
#
# $Id$
# ---------------------------------------------------------------------

PATH="$(dirname $0):$PATH"

usage="[ PGROOT=<root_directory> ]  $(basename $0) \n        [ {--nsegs|-n} { #segs | 1 } ] \n        [ {--segname|-s} { segname | segDB} ] \n        [ {--prefix|-P} { dbprefix | cdb} ] \n        [ {--useIP | -i} ] [ {--withTPCH }  {--withTiny} ] \n        [ {--verbose} ] [ {--help} ]"

echo -e "$(basename $0): Create and load a database into a CDB cluster."
echo -e "Copyright (c) 2004-2005 Metapa, Inc. All Rights Reserved"

function display_help
{
    echo -e "\nSyntax: $usage"

    echo -e "\nThis command allows you to create and load database(s) into a CDB cluster."
    echo    "The CDB cluster must have been previously initialized using \"cdbinitdb\" "
    echo    "Unless otherwise specified, the name of the database created is \"segDB\"."

    echo -e "\nIf \$PGROOT is not set, the root of the CDB cluster in which the database(s) "
    echo    "are created defaults to \"\$HOME/cdb-data\"."

    echo -e "\nYou can specify the number of segments instances you want to create in each"
    echo    "database by --nsegs or -N. If no value is set, then the default value is 1"
    echo    "The prefix of each segment name can be specified with --segname or -s. This "
    echo    "defaults to \"segDB\". If there are multiple segments all segment DBs will"
    echo    "have an ascending integer appended to their name. E.g segDB1, segDB2, etc."
    echo -e "\nIf there are are multiple listeners then the increment will carry across all"
    echo    "the listeners. E.g if there are two listeners and 3 segments per listener, then"
    echo    "segDB1 will be on the first listener, while segDB6 will be on the second."

    echo -e "\nBy default, the value inserted into the cdb.host table will be that returned"
    echo -e "by 'hostname'. If you prefer to specify your ip address then use --useIP or -i."
    echo    "NOTE: you will have to make sure your pg_hba.conf file(s) are correctly configured."

    echo -e "\nBy default, the CDB cluser will not have any user tables. If you wish, you can"
    echo    "preload the TPCH schema using the --withTPCH option. The DDL and additional files"
    echo    "required are found in the cdb-qa module. You will have to edit TPCH_load.ctl to"
    echo    "contain the correct path for your environment. NOTE: the data loaded is ALL"
    echo    "actually partitioned at this time. segDB1 will have NO data, and is the query "
    echo    "dispatcher. The other segDBs contain their slice of the data."

    echo -e "\nAlternatively, you can seed the cluster with the Tiny schema by using the"
    echo    "--withTiny option. Tiny schema is a very small set of tables and data that"
    echo    "can be used for testing purposes."

    echo -e "\nSee README.txt for more details.\n"

    exit 1
}

# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
#
# Pick out command line arguments:
#
#     -D | --dbroot     <root_directory>        $PGROOT
#     -P | --prefix     <dbprefix>
#     -N | --nsegs      <#segs>
#          --verbose    <verbose>
#          --withTPCH   <also load tpch schema>
#
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# Source our utility functions
source cdbfuncs

# Some useful variables
dir="$(dirname $0)"
hostname="localhost"
username="$(whoami)"
plain=false
positional=0
options_ended=
dbroot=
nsegs=
dbprefix=
segname=
verbose=false
ip=none
withTPCH=false
withTiny=false
entrySegName=

# These are the files needed for the TPCH schema
tpchDDL=../../cdb-qa/refdbs_cdb2/refdb01/TPCH_DDL.sql
tpchDistPolicy=../../cdb-qa/refdbs_cdb2/refdb01/TPCH_define_tables.sql
tpchData=../../cdb-qa/refdbs_cdb2/refdb01/TPCH_load.ctl
tpchConstraints=../../cdb-qa/refdbs_cdb2/refdb01/TPCH_KEYS.sql

# These are the files needed for the Tiny schema
cvpDDL=tiny/cvpschema.sql
cvpDistPolicy=tiny/cvphead.sql
cvpData=tiny/cvpdata.sql

# Ok, loop through the command line args
#
# TODO: For some reason, the -X form of command line parsing, does not handle
# errors very well. Needs fixing at some point.
while [ $# -gt 0 ]
do
    if [ ! $options_ended ]; then
        OPTIND=1
        OPTARG='--'
        while getopts :D:s:n:P:i: option
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
                s)
                   segname="$OPTARG"
                   ;;
                i)
                   ip="$OPTARG"
                   ;;
                n)
                   nsegs="$OPTARG"
                   ;;
                P)
                   dbprefix="$OPTARG"
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
            --nsegs)
                nsegs=${2:?Option '--nsegs' requires a value}
                shift 2
                ;;
            --nsegs=*)
                nsegs=${val:?Option '--nsegs' requires a value}
                shift 1
                ;;
            --segname)
                segname=${2:?Option '--segname' requires a value}
                shift 2
                ;;
            --segname=*)
                segname=${val:?Option '--segname' requires a value}
                shift 1
                ;;
            --prefix)
                dbprefix=${2:?Option '--prefix' requires a value}
                shift 2
                ;;
            --prefix=*)
                dbprefix=${val:?Option '--prefix' requires a value}
                shift 1
                ;;
            --useIP)
                ip=${2:?Option '--prefix' requires a value}
                shift 1
                ;;
            --useIP=*)
                ip=${val:?Option '--useIP' requires a value}
                shift 1
                ;;
            --verbose)
                verbose=true
                shift 1
                ;;
            --withTPCH)
                withTPCH=true
                shift 1
                ;;
            --withTiny)
                withTiny=true
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

# Did we get a valid number of segments?
if [ "$nsegs" ]
then
    if expr "$nsegs" : '[1-9][0-9]*$' 1>/dev/null
    then
       :
    else
      echo "The number of segments must be specified as a number" >&2
      exit 2
    fi
else
    # If nothing is specified, then default to 3
    : ${nsegs:=1}
fi

# And what about our prefix?
dbprefix=${dbprefix:-$dbprefix}
: ${dbprefix:=cdb}

# And what about the segname?
segname=${segname:-$segname}
: ${segname:=segDB}

# And what about the root dir?
PGROOT=${dbroot:-$PGROOT}
: ${PGROOT:=$HOME/cdb-data}

# Should we use the ip address, rather than localhost?
if [ "$ip" != "none" ]
then
  hostname="$ip"
fi

# Did they want the TPCH schema?
if [ "$withTPCH" = "true" ]
then
  # Check for the existence of the files
  dirname=$(dirname $tpchDDL)

  if [ ! -d $dirname ]
  then
    echo -e "\nERROR: Unable to access \"$dirname\"."
    echo -e "This is where we expect to find the TPCH schema. You probably "
    echo -e "need to check out the cdb-qa module."
    exit -1
  fi
fi

if [ ! "$PGDATABASE" ]; then
    export PGDATABASE=${1:-segDB}
else
    export PGDATABASE=${1:-$PGDATABASE}
fi

# Discard outside values of these
unset PGHOST
unset PGHOSTADDR

# Set this value
PGOPTIONS='-c cdb_session_role=utility'

# The following vary depending on the target node
export PGDATA
export PGPORT
export PGOPTIONS

shopt -s extglob
schemaUpdates="/tmp/$$.sql"
(( instancecount=0 ))

# How many db instances are there?
if [ "$verbose" = "true" ]
then
    echo -e "\nSearching for databases in $PGROOT"
    echo -e "Using dbprefix of $dbprefix"
    echo -e "Found:"
fi

# First of all, count up how many databases we can find
for PGDATA in "$PGROOT"/$dbprefix*[0-9]?([0-9])
do
    [ ! -d "$PGDATA" ] && continue
    (( instancecount++ ))
    if [ "$verbose" = "true" ]
    then
        echo -e "  $PGDATA"
    fi
done

# Did we find any?
if [ $instancecount -gt 0 ]
then
  echo -e "\nFound $instancecount listener instance(s) in $PGROOT"
else
  echo -e "\nUnable to find any listener instances in $PGROOT using a prefix of $dbprefix"
  exit 1
fi

# Initialise some arrays we will use to store the admin statements
declare -a listeners_array
declare -a segments_array

# And some other useful variables
current_listener=0
current_segment=0

#
# Ok, we have some databases, let's loop through them and create the segment
# databases.
#
# This loop also serves to generated and store some other cdb config data into
# some arrays for later use.
#
for PGDATA in "$PGROOT"/$dbprefix[0-9]?([0-9])
do
    [ ! -d "$PGDATA" ] && continue
    PGDATA=$(cd "$PGDATA"; pwd)
    PGPORT=$(getPort)

    if [ "$verbose" = "true" ]
    then
        echo -e "About to create a seg DB in $PGDATA, Port = $PGPORT\n  "
        echo -e "Number of segments to create = $nsegs"
    fi

    # This is the loop for each segment database
    for (( i = 1; i <= nsegs; i++ ))
    do

      # Do we need to append anything to the segname?
      finalSegName=$segname
      if [ $nsegs -gt 0 ]
      then
          finalSegName=$finalSegName$current_segment
          PGDATABASE=$finalSegName
      fi

      echo -e "\nCreating ${PGDATABASE} in $PGDATA"

      db=$(psql -t --list template1 2>/dev/null | grep "^[[:space:]]${PGDATABASE}[[:space:]]")

      if [ "$db" ]
      then
          sleep 1
          echo "A database called $PGDATABASE already exists, so dropping it."
          dropdb $PGDATABASE
      else
          echo -e "Found no DB named $PGDATABASE, no need to DROP"
      fi

      sleep 1
      createdb

      # Store the listener statements
      listeners_array[current_listener]="select cdb.add_listener('$hostname', $PGPORT, '$PGDATA', '$username', NULL);"

      # And our segment statements
      if [ "$current_segment" -gt 0 ]
      then
        segdb_array[current_segment]="select cdb.add_segment_database('$finalSegName', '$hostname', $PGPORT, true, $((current_segment-1)));"
      fi

      # Increment our segment counter
      (( current_segment++ ))
    done

    # Increment our listener counter
    (( current_listener++ ))

done

# Show everyone what will be inserted into the entry database
if [ "$verbose" = "true" ]
then
  for i in "${listeners_array[@]}"; do
    echo "Listener : $i"
  done

  for i in "${segdb_array[@]}"; do
    echo "Seg DB : $i"
  done
fi

# Reset our counter
current_listener=0
current_segment=0
total_content_segs=${#segdb_array[@]}
entrySegName=$segname$current_listener

#
# Next loop. Here we record the identity of the current segment in the local
# cdb.cdb table. Optionally, we also load and prune either the TPCH or Tiny
# schema into the segment databases
#
# NOTE: Currently we have to load the actual schema and data PRIOR to filling
# in the admin schema because of the oid inconsistency problem.
for PGDATA in "$PGROOT"/$dbprefix[0-9]?([0-9])
do
    [ ! -d "$PGDATA" ] && continue
    PGDATA=$(cd "$PGDATA"; pwd)
    PGPORT=$(getPort)

    # This is the loop for each segment database
    for (( loop = 0; loop < $nsegs; loop++ ))
    do

      # Do we need to append anything to the segname?
      finalSegName=$segname
      if [ $nsegs -gt 0 ]
      then
          finalSegName=$finalSegName$current_segment
          PGDATABASE=$finalSegName
      fi

      dbid=$((current_segment+1));
      contentid=$((current_segment-1));

      # And finally, set the identity
      echo -e "\nPopulating identity table of $finalSegName on port = $PGPORT"

      if [ "$verbose" == "true" ]
      then
        echo "Inserting into identity table: num segs $total_content_segs, db_id $dbid, content_id $contentid";
      fi

      psql -o /dev/null -q $finalSegName -c "select cdb.cdb_set_identity('Default Cluster', '$total_content_segs', $dbid, $contentid);"

      if [ "$withTPCH" = "true" ]
      then

        # Load the DDL
        echo "Loading TPCH DDL"
        psql -o /dev/null -q $finalSegName -f $tpchDDL > /dev/null 2>&1

        # Is this the entry database?
        if [ "$entrySegName" == "$finalSegName" ]
        then
          echo "Setting TPCH distribution policy"
          psql -o /dev/null -q $finalSegName -f $tpchDistPolicy
        fi

        echo "Loading data into TPCH tables"
        psql -o /dev/null -q $finalSegName -f $tpchData

        if [ "$finalSegName" != "$entrySegName" ]
        then
          echo "Pruning data from TPCH tables"

          if [ "$verbose" == "true" ]
          then
            echo -e "Segname = $finalSegName, total segs = $total_content_segs, content id = $contentid\n";
          fi

          psql $finalSegName -c "delete from tpch.lineitem where l_orderkey % $total_content_segs <> $contentid"
          psql $finalSegName -c "delete from tpch.orders where o_orderkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.part where p_partkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.region where r_regionkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.nation where n_nationkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.supplier where s_suppkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.partsupp where ps_partkey % $total_content_segs <> $contentid "
          psql $finalSegName -c "delete from tpch.customer where c_custkey % $total_content_segs <> $contentid "
          echo "Finished Pruning data from TPCH tables"
        fi

        echo "Setting table constraints"
        psql -o /dev/null -q $finalSegName -f $tpchConstraints > /dev/null 2>&1

        if [ "$finalSegName" == "$entrySegName" ]
        then
          echo "Deleting data from entry database"
          psql $finalSegName -c "delete from tpch.lineitem"
          psql $finalSegName -c "delete from tpch.orders"
          psql $finalSegName -c "delete from tpch.part"
          psql $finalSegName -c "delete from tpch.region"
          psql $finalSegName -c "delete from tpch.nation"
          psql $finalSegName -c "delete from tpch.supplier"
          psql $finalSegName -c "delete from tpch.partsupp"
          psql $finalSegName -c "delete from tpch.customer"
        fi
      fi

      if [ "$withTiny" = "true" ]
      then

        # Load the DDL
        echo "Loading Tiny DDL"
        psql -o /dev/null -q $finalSegName -f $cvpDDL > /dev/null 2>&1

        # Is this the entry database?
        if [ "$entrySegName" == "$finalSegName" ]
        then
          echo "Setting Tiny distribution policy"
          psql -o /dev/null -q $finalSegName -f $cvpDistPolicy
        fi

        # Load the data
        echo "Loading data into Tiny tables"
        psql -o /dev/null -q $finalSegName -f $cvpData

        # If this is not the entry db, prune the data
        if [ "$finalSegName" != "$entrySegName" ]
        then
          echo "Pruning data from Tiny tables"

          if [ "$verbose" == "true" ]
          then
            echo -e "Segname = $finalSegName, total segs = $total_content_segs, content id = $contentid\n";
          fi

          psql $finalSegName -c "delete from customer where cn % $total_content_segs <> $contentid"
          psql $finalSegName -c "delete from vendor where vn % $total_content_segs <> $contentid"
          psql $finalSegName -c "delete from product where pn % $total_content_segs <> $contentid"
          psql $finalSegName -c "delete from sale where cn % $total_content_segs <> $contentid"
          echo "Finished Pruning data from Tiny tables"
        fi

        echo -e "\nPerformaing vacuum on all tables"
        psql $finalSegName -c "vacuum analyze customer"
        psql $finalSegName -c "vacuum analyze vendor"
        psql $finalSegName -c "vacuum analyze product"
        psql $finalSegName -c "vacuum analyze sale"

        # If this is the entry db, blow the data away
        if [ "$finalSegName" == "$entrySegName" ]
        then
          echo "Deleting data from entry database"
          psql $finalSegName -c "delete from customer"
          psql $finalSegName -c "delete from vendor"
          psql $finalSegName -c "delete from product"
          psql $finalSegName -c "delete from sale"
        fi
      fi

      # Increment our segment counter
      (( current_segment++ ))

    done
done


#
# Ok, let's populate the admin schema in the entry listener
#
for PGDATA in "$PGROOT"/$dbprefix[0]?([0])
do
    [ ! -d "$PGDATA" ] && continue
    PGDATA=$(cd "$PGDATA"; pwd)
    PGPORT=$(getPort)

    echo -e "\nPopulating admin schema in entry listener in $entrySegName on port = $PGPORT\n"

    # Update the host table
    echo "Updating the host table. Host is $hostname";
    psql -o /dev/null -q $entrySegName -c "select cdb.add_host('$hostname');"

    # Now the listener table
    echo -e "Updating listener table with listeners"

    for i in "${listeners_array[@]}"; do
      if [ "$verbose" == "true" ]
      then
        echo -e " $i"
      fi

      psql -o /dev/null -q $entrySegName -c "$i"
    done

    # Add an entry database
    echo "Adding $listenerSeg as the entry database";
    psql -o /dev/null -q $entrySegName -c "select cdb.add_entry_database('$entrySegName', '$hostname', $PGPORT, true);"

    # And the segment database
    echo -e "Updating segment database table"

    for i in "${segdb_array[@]}"; do
      if [ "$verbose" == "true" ]
      then
        echo -e " $i"
      fi

      psql -o /dev/null -q $entrySegName -c "$i"
    done

done

exit 1
