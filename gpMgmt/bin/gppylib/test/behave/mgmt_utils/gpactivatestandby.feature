@gpactivatestandby
Feature: gpactivatestandby stop segments
    @wip
    Scenario: Validate gpactivatestandby stop's segments in fast mode
        Given the database is running
        And the database "gpstandbydb" does not exist
        And database "gpstandbydb" exists
        When the user initializes standby master on "smdw"
        Then gpinitstandby should return a return code of 0
        Given there is a "heap" table "public.test_table" with compression "None" in "gpstandbydb" with data
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        And the numbers "1" to "1000000" are inserted into "public.test_table" tables in "gpstandbydb"
        When the user runs the query "select count(*) from public.test_table a, public.test_table b, public.test_table c, public.test_table d, public.test_table e" on "gpstandbydb"
        And the user runs "gpstop -a -m -M fast" 
        Then gpstop should return a return code of 0
        When the performance timer is started
        And the user activates standby on the standbyhost
        Then gpactivatestandby should return a return code of 0
        And the performance timer should be less then "300" seconds
        When the user initializes standby master on "mdw"
        Then gpinitstandby should return a return code of 0
        And the user runs the command "gpstop -a -m -M fast" from standby
        And gpstop should return a return code of 0
        When the user runs "gpactivatestandby -d $MASTER_DATA_DIRECTORY -fa"
        Then gpactivatestandby should return a return code of 0
        And database "gpstandbydb" health check should pass on table "public.test_table"
        And the standby is initialized if required
