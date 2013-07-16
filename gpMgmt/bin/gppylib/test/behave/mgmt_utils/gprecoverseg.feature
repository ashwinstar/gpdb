@gprecoverseg
Feature: gprecoverseg tests
    Scenario: gprecoverseg should not output bootstrap error on success
        Given the database is running
        And user kills a primary postmaster process
        And user can start transactions
        When the user runs "gprecoverseg -a"
        Then gprecoverseg should return a return code of 0
        And gprecoverseg should not print Unhandled exception in thread started by <bound method Worker.__bootstrap to stdout
        And the segments are synchronized
        When the user runs "gprecoverseg -ra"
        Then gprecoverseg should return a return code of 0
        And gprecoverseg should not print Unhandled exception in thread started by <bound method Worker.__bootstrap to stdout
         
