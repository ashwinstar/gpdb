*******************************************************************************
ABOUT JETPACK
*******************************************************************************

Jetpack is a set of administrative functions and views that are used
as an administrative interface to Greenplum Database. The SQL code 
in the jetpack.sql file creates the pl/pgsql language, several external tables, 
functions and views that can then be queried by the Greenplum administrator 
to show the state of a running Greenplum Database system. 

* NOTE: To install and use Jetpack, you must be a database superuser.


******************************************************************************
INSTALLING JETPACK
******************************************************************************

1. Execute the jetpack.sql file as a database superuser, where 
    gpadmin is the superuser name and mydatabase# are the databases
    in which you want to install the Jetpack schema and objects. If your 
    Greenplum system has multiple databases, you must install the Jetpack 
    schema into each database. For example:
 
    psql -f $GPHOME/lib/jetpack/jetpack.sql -U gpadmin -d mydatabase1
    
    psql -f $GPHOME/lib/jetpack/jetpack.sql -U gpadmin -d mydatabase2

2. For convenience, add the gp_jetpack schema to your schema search 
    path. For example (where myschema1 and myschema2 are names of 
    your user-created schemas):

    psql -c "ALTER DATABASE mydatabase1 SET search_path \
    TO myschema1,myschema2,gp_jetpack,pg_catalog" \
    -U gpadmin -d template1


******************************************************************************
JETPACK VIEWS
******************************************************************************
Jetpack contains the following views that an administrator can query 
to find out various system state information. These views access underlying
Jetpack external tables and functions, and must be accessed by a superuser.

   ***********************************************************************
    VIEW NAME: gp_bloat_diag 

    DESCRIPTION: Shows tables that have bloat (the actual number of pages
                 on disk exceeds the expected number of pages given the
                 table statistics). Tables that are bloated require a 
                 VACUUM or a VACUUM FULL in order to reclaim disk space. 

    COLUMNS: 
    * bdirelid - table object id
    * bdinspname - schema name
    * bdirelname - table name
    * bdirelpages - actual number of pages on disk
    * bdiexppages - expected number of pages given the table data
    * bdidiag - bloat diagnostic message

   
   ***********************************************************************

    VIEW NAME: gp_bloat_expected_pages

    DESCRIPTION: Shows the actual number of pages on disk as compared to 
                 the expected number of pages given the table statistics.

    COLUMNS: 
    * btdrelid - table object id
    * btdrelpages - actual number of pages on disk
    * btdexppages - expected number of pages given the table data


   **********************************************************************

    VIEW NAME: gp_log_command_timings

    DESCRIPTION: Reads the log file on the master and reports the 
                 execution time of a statement.

    COLUMNS: 
    * logsession - The session identifier (prefixed with "con")
    * logcmdcount - The command number within a session (prefixed with "cmd")
    * logdatabase - The name of the database
    * loguser - The name of the database user
    * logpid - The process id (prefixed with "p")
    * logtimemin - The time of the first log message for this command.
    * logtimemax - The time of the last log message for this command.
    * logduration - Statement duration from start to end time.


   **********************************************************************

    VIEW NAME: gp_log_database

    DESCRIPTION: Reads the server log files of the entire Greenplum system
                 (master, segments, and mirrors) and reports all log 
                 entries found for the current database. Associated log 
                 entries can be identified by the session id (logsession) 
                 and command id (logcmdcount).

    COLUMNS: 
     * logtime - The timestamp of the log message
     * loguser - The name of the database user
     * logdatabase - The name of the database
     * logpid - The associated process id (prefixed with "p")
     * logthread - The associated thread count (prefixed with "th")
     * loghost - The segment or master host name  
     * logport -  The segment or master port
     * logsessiontime - Time session connection was opened
     * logtransaction - Global transaction id
     * logsession - The session identifier (prefixed with "con")
     * logcmdcount - The command number within a session (prefixed with "cmd")
     * logsegment - The segment content identifier (prefixed with "seg" 
                    for primary or "mir" for mirror. The master always 
                    has a content id of -1.)
     * logslice - The slice id (portion of the query plan being executed)
     * logdistxact - Distributed transaction id
     * loglocalxact - Local transaction id
     * logsubxact - Subtransaction id
     * logseverity - LOG, ERROR, FATAL, PANIC, DEBUG1 or DEBUG2
     * logstate - SQL state code associated with the log message
     * logmessage - Log or error message text
     * logdetail - Detail message text associated with an error message
     * loghint - Hint message text associated with an error message
     * logquery - The internally-generated query text
     * logquerypos - The cursor index into the internally-generated query text
     * logcontext - The context in which this message gets generated
     * logdebug - Query string with full detail for debugging
     * logcursorpos - The cursor index into the query string
     * logfunction - The function in which this message is generated
     * logfile - The log file in which this message is generated
     * logline - The line in the log file in which this message is generated
     * logstack - Full text of the stack trace associated with this message


   ************************************************************************

    VIEW NAME: gp_log_master_concise

    DESCRIPTION: Reports a subset of the log fields from the master lof file.

    COLUMNS: 
     * logtime - The timestamp of the log message
     * logdatabase - The name of the database
     * logsession - The session identifier (prefixed with "con")
     * logcmdcount - The command number within a session (prefixed with "cmd")
     * logmessage - Log or error message text

   ************************************************************************

    VIEW NAME: gp_log_system

    DESCRIPTION: Reads the server log files of the entire Greenplum system
                 (master, segments, and mirrors) and reports all log entries 
                 found for the entire system. Associated log entries can be 
                 identified by the session id (logsession) and command id 
                 (logcmdcount).

    COLUMNS: 
     * logtime - The timestamp of the log message
     * loguser - The name of the database user
     * logdatabase - The name of the database
     * logpid - The associated process id (prefixed with "p")
     * logthread - The associated thread count (prefixed with "th")
     * loghost - The segment or master host name  
     * logport -  The segment or master port
     * logsessiontime - Time session connection was opened
     * logtransaction - Global transaction id
     * logsession - The session identifier (prefixed with "con")
     * logcmdcount - The command number within a session (prefixed with "cmd")
     * logsegment - The segment content identifier (prefixed with "seg" 
                    for primary or "mir" for mirror. The master always has 
                    a content id of -1.)
     * logslice - The slice id (portion of the query plan being executed)
     * logdistxact - Distributed transaction id
     * loglocalxact - Local transaction id
     * logsubxact - Subtransaction id
     * logseverity - LOG, ERROR, FATAL, PANIC, DEBUG1 or DEBUG2
     * logstate - SQL state code associated with the log message
     * logmessage - Log or error message text
     * logdetail - Detail message text associated with an error message
     * loghint - Hint message text associated with an error message
     * logquery - The internally-generated query text
     * logquerypos - The cursor index into the internally-generated query text
     * logcontext - The context in which this message gets generated
     * logdebug - Query string with full detail for debugging
     * logcursorpos - The cursor index into the query string
     * logfunction - The function in which this message is generated
     * logfile - The log file in which this message is generated
     * logline - The line in the log file in which this message is generated
     * logstack - Full text of the stack trace associated with this message


   **************************************************************************

    VIEW NAME: gp_resq_activity

    DESCRIPTION: For systems that are using resource queues for workload
                 management, this view shows the status of statements submitted
                 to the system via a resource queue. Note that statements 
                 issued by superusers or users not assigned to a queue are 
                 exempt from resource queuing.

    COLUMNS: 
     * resqprocpid - Process id
     * resqrole - User name
     * resqoid - Resource queue object id
     * resqname - Resource queue name
     * resqstart - Time statement was issued to the system
     * resqstatus - Status of statement: running, waiting or cancelled


   **************************************************************************

    VIEW NAME: gp_resq_activity_by_queue

    DESCRIPTION: Shows the overall status for each resource queue in the system.

    COLUMNS: 
     * resqoid - Resource queue object id
     * resqname - Resource queue name
     * resqlast - Time of the last statement issued to the queue
     * resqstatus - Status of last statement: running, waiting or cancelled
     * resqtotal - Total statements in this queue

   ***************************************************************************

    VIEW NAME: gp_skew_coefficients

    DESCRIPTION: Shows the coefficient of variation (CV) calculation for
                 the data stored on each segment.

    COLUMNS: 
     * skcoid - The object id of the table
     * skccoeff - The coefficient of variation (CV) is calculated as the 
                  standard deviation divided by the average. It takes into 
                  account both the average and variability around the average 
                  of a data series. The lower the value, the better. Higher
                  values indicate greater data skew.

   ****************************************************************************

    VIEW NAME: gp_skew_idle_fractions

    DESCRIPTION: Shows the percentage of the system that is idle during a 
                 table scan, which is an indicator of uneven data distribution.

    COLUMNS: 
     * sifoid - The object id of the table
     * siffraction - The percentage of the system that is idle during a table
                     scan, which is an indicator of uneven data distribution
                     or skew. For example, a value of 0.1 indicates 10% skew,
                     a value of 0.5 indicates 50% skew, and so on. Tables that
                     have more than 10% skew should have their distribution 
                     policies evaluated.

   ****************************************************************************

    VIEW NAME: gp_stats_missing

    DESCRIPTION: Shows tables that do not have statistics and therefore 
                 require an ANALYZE be run on the table.

    COLUMNS: 
     * smischema - Schema name
     * smitable - Table name
     * smisize - Table size
     * smicols - Number of columns
     * smirecs - Number of rows

   ****************************************************************************


*******************************************************************************
JETPACK FUNCTIONS
*******************************************************************************

Jetpack contains the following functions that an administrator can call directly.
There are a number of other functions created in the Jetpack schema that are
intended to be called by the associated views described above, and are not 
described in this documentation. 

   **************************************************************************

    FUNCTION NAME: gp_param_setting('parameter_name')

    DESCRIPTION: When passed the name of a server configuration parameter,
                 returns a row of the following fields for each segment:
                 * paramsegment - The segment content id (only active segments 
                                are shown). The master content id is always -1.
                 * paramname - The name of the configuration parameter.
                 * paramvalue - The value set for that parameter.

   EXAMPLE: 
            SELECT * FROM gp_param_setting('max_connections');


   **************************************************************************
    
    FUNCTION NAME: gp_jetpack_version()

    DESCRIPTION: Shows the version of the gp_jetpack schema.

    EXAMPLE: 
            SELECT gp_jetpack_version();

  ***************************************************************************


*****************************************************************************
JETPACK EXTERNAL TABLES
*****************************************************************************

Jetpack contains the following external table that an administrator can use
to check free disk space on segment hosts. There are a number of other 
external tables created in the Jetpack schema that are intended to be called 
by the associated views described above, and are not described in this 
documentation. 


TABLE NAME: gp_disk_free

DESCRIPTION: Runs the df command on the segment hosts and reports
             back the results. Can only be accessed by superusers.

COLUMNS: 
 * dfsegment - Active segment content id
 * dfspace - Free disk space on the segment






   