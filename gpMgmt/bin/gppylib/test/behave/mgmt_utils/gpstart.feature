@gpstart
Feature: Validate command line arguments

    Scenario: Entering an invalid argument
        When the user runs "gpstart -z"
        Then gpstart should print no such option: -z error message 
        And gpstart should return a return code of 2

    Scenario: Run gpstart
        Given the database is not running
        When the user runs "gpstart -a"
        Then gpstart should return a return code of 0
        And database "testdb" health check should pass on table "t1"

    @dca
    Scenario: gpstart with 1 host down
        Given we have determined the first segment hostname
        And the database is not running
        And eth2 on the first segment host is down
        And eth3 on the first segment host is down
        When the user runs "gpstart -a"
        Then gpstart should return a return code of 1
        And database "testdb" health check should pass on table "t1"
        And eth2 on the first segment host is up
        And eth3 on the first segment host is up
        And the user runs "gprecoverseg -a"
        And the segments are synchronized
        And the user runs "gprecoverseg -r"
        And the segments are synchronized

    Scenario: Remove MASTER_DATA_DIRECTORY from os.environ
        Given the database is not running
        And MASTER_DATA_DIRECTORY environment variable is not set
        When the user runs "gpstart -a"
        Then gpstart should return a return code of 2
        And gpstart should print Environment Variable MASTER_DATA_DIRECTORY not set! to stdout
        And MASTER_DATA_DIRECTORY environment variable should be restored
        
    
    @wip
    Scenario: Standby syncmaster is running with no pgrep command on standby
        Given the database is running
        and all the segments are running
        and the segments are synchronized
        When the user initializes standby master on "smdw"
        Then gpinitstandby should return a return code of 0
        And the user runs "gpstop -am" 
        And gpstop should return a return code of 0
        When the user removes the "pgrep" command on standby
        And the user runs "gpstart -a" 
        Then gpstart should return a return code of 0
        And gpstart should print command "pgrep" is not found to stdout
        And the user restores the "pgrep" command on the standby

