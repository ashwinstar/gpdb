=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
- CDB Utility Scripts                                                         -
-                                                                             -
- Authors: Todd Papaioannou, Clifford Johnson                                 -
- Version: $Revision$                                                   -
=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

The following BASH scripts make up a set of "utility" tools for creating
and interacting with a local CDB cluster. The intention is that developers
and testers will be able to use this cluster to perform their work. These
scripts are not intended for any type of production environment.

    cdbinitdb   --  initializes new CDB database cluster
                    data directory(s) (like initdb).

    cdbstart    --  starts the amsd and listener(s) for a CDB database
                    cluster

    cdbcreatedb --  creates a new database in a CDB database
                    cluster (like createdb).

    cdbstop     --  stops the amsd and listener(s) for a CDB database
                    cluster

    cdbsql      --  runs psql connecting to the first listener of
                    the cluster

    cdbclean    --  deletes the CDB cluster database directory(s)
                    that were created using cdbinitdb

    cdbfuncs    --  contains functions for use in the other scripts

Note: all the scripts support a --help option that will give further details
of the options and behaviour of each script.

Overview
========

These scripts will allow you to create and work with one or several CDB
clusters all residing on the SAME host. The following sections describe
the default behaviour of these scripts. Nearly all of the defaults can be
overridden by using the command line options to each script. Use the
--help option to obtain instructions for each script. In particular, pay
attention to the --prefix option, which will allow you to work with multiple
CDB instances.

The cdbinitdb script initializes a single PostgreSQL instance and
associated listener. By default, this occurs in $HOME/cdb-data/cdb
and the default port is 10000. Multiple listeners _can_ be configured
using the script though, and in this case, port numbers are incremented
from the base port. The postgresql.conf file of each instance is
altered to set the port (as described), set max_connections to 10,
and set shared_buffers to 32.

The cdbcreatedb script creates databases within the PostgresQL instances
previously created using cdbinitdb. By default, three segment databases
are created for each listener instance. This can be configured on the command
line using the --nsegs argument. If there are multiple segments all segment
DBs will have an ascending integer appended to their name. E.g segDB1, segDB2,
etc.

If there are are multiple listeners then the increment will carry across all
the listeners. E.g if there are two listeners and 3 segments per listener, then
segDB1 will be on the first listener, while segDB6 will be on the second.

Example
=======

To create a new CDB cluster, do the following:

    # Set up the required environmental variables. Each of
    # the cdbXXXXX scripts uses these values as needed.

    $ source ./env.rc


    # Initialize the CDB cluster data directories;  if the directory
    # identified in PGROOT doesn't exist, cdbinitdb creates it.

    $ ./cdbinitdb


    # Start the CDB cluster to enable creation of segment database(s)

    $ ./cdbstart


    # Create the segment databases. If you omit a database name, then
    # then it will be created with the name of segDB#

    $ ./cdbcreatedb


    # Connect to the CDB cluster using cdbpsql

    $ ./cdbsql segDB1

Continue working with your CDB cluster as you see fit!

By default, the CDB cluser will not have any user tables. If you wish, you can
preload the TPCH schema using the --withTPCH option when executing cdbcreatedb.
The DDL and additional files required are found in the cdb-qa module. You will
have to edit TPCH_load.ctl to contain the correct path for your environment.
NOTE: the data loaded is NOT actually partitioned at this time. Each segDB will
contain the same data.

Notes
=====

The bin directory of the CDB PostgreSQL code base is expected to be in
$PATH. There are a number of other environmental variables that are
useful. You can source env.rc to automatically set these up.

A word of WARNING -- these scripts are created for internal use; little,
if any, error checking is performed.  It is expected that the typical
recovery action is to kill the cluster's listeners, wipe out the PGROOT
directory, and start over from scratch. Luckily, cdbclean is provided to
do this ;)
