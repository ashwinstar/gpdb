@misc
Feature: Miscellaneous tests which do not belong to mgmt utilities

    Scenario: Set size of current_query in pg_stat_activity table to a valid value (1024 - INT_MAX) 
        Given the database is running
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 1050 --masteronly"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs "psql -c 'show pgstat_track_activity_query_size' template1"
        Then psql should print 1050 to stdout 
        
    Scenario: Set size of current_query in pg_stat_activity table to invalid value 
        Given the database is running
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 10 --masteronly"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs "psql -c 'show pgstat_track_activity_query_size' template1"
        Then psql should print 1024 to stdout 

    Scenario: Set size of current_query in pg_stat_activity table to large invalid value 
        Given the database is running
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 30000000000 --masteronly"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs "psql -c 'show pgstat_track_activity_query_size' template1"
        Then psql should print 1024 to stdout 

    @wip
    Scenario: Large query with number of chars greater than current_query size in pgstat_activity in CT mode
        Given the database is running
        And the database "testdb" does not exist
        And database "testdb" exists
        And user kills a primary postmaster process
        And user can start transactions
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 10000"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs command "PGOPTIONS=' -c gp_session_role=utility' psql -c 'show pgstat_track_activity_query_size' template1" on the "Change Tracking" segment
        Then psql should print 10000 to stdout 
        When the user runs "gprecoverseg -a"
        Then gprecoverseg should return a return code of 0
        And the segments are synchronized
        And user can start transactions
        And the user runs "gprecoverseg -r -a"
        And gprecoverseg should return a return code of 0
        And the segments are synchronized
        And user can start transactions
        When the user runs command "PGOPTIONS=' -c gp_session_role=utility' psql -c 'show pgstat_track_activity_query_size' template1" on the "Original" segment
        Then psql should print 10000 to stdout 
        And the segments are synchronized

    Scenario: Large query with number of chars greater than current_query size in pgstat_activity
        Given the database is running
        And the database "testdb" does not exist
        And database "testdb" exists
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 10000 --masteronly"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs "psql -c 'show pgstat_track_activity_query_size' template1"
        Then psql should print 10000 to stdout 
        When the user runs the query "create table aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa (i int)" on "testdb"
        And the user runs the query "insert into aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa select * from generate_series(1, 1000)" on "testdb"
        And the user runs query from the file "gppylib/test/behave/mgmt_utils/steps/data/long_query1.sql" on "testdb" without fetching results 
        When the user runs "psql -c 'select current_query from pg_stat_activity' template1"
        Then the text in the file "gppylib/test/behave/mgmt_utils/steps/data/long_query1.out" should be printed to stdout
        And the user runs "gpstop -M fast -a"

    Scenario: Large query with number of chars lesser than current_query size in pgstat_activity
        Given the database is running
        And the database "testdb" does not exist
        And database "testdb" exists
        And the user runs "gpconfig -c pgstat_track_activity_query_size -v 10000 --masteronly"
        And gpconfig should return a return code of 0
        And the user runs "gpstop -ar"
        And gpstart should return a return code of 0
        When the user runs "psql -c 'show pgstat_track_activity_query_size' template1"
        Then psql should print 10000 to stdout 
        When the user runs the query "create table aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa (i int)" on "testdb"
        And the user runs the query "insert into aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa select * from generate_series(1, 1000)" on "testdb"
        And the user runs the following query on "testdb" without fetching results 
            """select count(*) from aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa c, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa d, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa e, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa f, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa g, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa h, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa j, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa k, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa l, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa m, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa n, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa o, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa p, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa q, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa r, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa s, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa t, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa u""" 
        When the user runs "psql -c 'select current_query from pg_stat_activity' template1"
        Then the following text should be printed to stdout 
            """select count\(\*\) from aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa b, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa c, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa d, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa e, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa f, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa g, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa h, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa j, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa k, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa l, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa m, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa n, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa o, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa p, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa q, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa r, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa s, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa t, aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa u"""
        And the user runs "gpstop -M fast -a"
