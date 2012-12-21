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
        And database health check should pass

    Scenario: gpstart with 1 host down
        Given we have determined the first segment hostname
        And the database is not running
        And eth2 on the first segment host is down
        And eth3 on the first segment host is down
        When the user runs "gpstart -a"
        Then gpstart should return a return code of 1
        And database health check should pass
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
        
    
