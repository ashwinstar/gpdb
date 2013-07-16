@gpload
Feature: gpload tests

    Scenario: gpload -l option should work even when the user does not have write permissions on home directory 
        Given the database is running
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "t1" with compression "None" in "testdb" with data
        And the user truncates "public.t1" tables in "testdb"
        And the gpAdminLogs directory has been backed up
        And the user does not have "write" permission on the home directory
        When the user runs "gpload -f gppylib/test/behave/mgmt_utils/steps/data/gpload_ctl_fmt.yml -l /tmp/gpload.log" 
        Then gpload should return a return code of 0
        And the "file" path "/tmp/gpload.log" should " " exist
        And the "directory" path "~/gpAdminLogs" should "not" exist

    Scenario: gpload without -l option should create log file in default location 
        Given the database is running
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "t1" with compression "None" in "testdb" with data
        And the user truncates "public.t1" tables in "testdb"
        And the gpAdminLogs directory has been backed up
        When the user runs "gpload -f gppylib/test/behave/mgmt_utils/steps/data/gpload_ctl_fmt.yml" 
        Then gpload should return a return code of 0
        And the "directory" path "~/gpAdminLogs" should " " exist
        And the directory "~/gpAdminLogs" is removed
