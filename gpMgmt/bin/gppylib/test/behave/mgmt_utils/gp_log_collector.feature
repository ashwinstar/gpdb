@gp_log_collector
Feature: Validate command line arguments

    Scenario: Entering an invalid argument
        Given the database is running
        When the user runs "gp_log_collector --hulu"
        Then gp_log_collector should return a return code of 2

    Scenario: Enter invalid argument to node collector
        Given the database is running
        When the user runs sbin command "nodecollector.py --hulu"
        Then nodecollector.py should return a return code of 2

    @dca
    Scenario: Host address override test
        Given the database is running
        And the directory /tmp/gp_log_collect is removed or does not exist
        When the user runs "gp_log_collector -t /tmp/gp_log_collect -l /tmp/gp_log_collect -S sdw1-2,sdw2-2,sdw3-2,sdw4-2,sdw5-2,sdw6-2,sdw7-2,sdw8-2,sdw9-2,sdw10-2,sdw11-2,sdw12-2,sdw13-2,sdw14-2,sdw15-2,sdw16-2"
        Then gp_log_collector should return a return code of 0
        And the directory /tmp/gp_log_collect exists
        And pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed or does not exist

    @logdir
    Scenario: Handle log_dir creation failures
        When the user runs "gp_log_collector -l /dev/null/baddirectory -t /tmp/gp_log_collect"
        then gp_log_collector should print Failed to create or set permissions on to stdout

    @tempdir
    Scenario: Handle tempDir creation failures on nodecollector
        When the user runs "gp_log_collector -l /tmp/gp_log_collect -t /dev/null/baddirectory"
        then gp_log_collector should print \[ERROR\]:-Exception: to stdout

    @install
    Scenario: Install and run gp_log_collector
        Given the database is running
        And the directory /tmp/gp_log_collect is removed or does not exist
        When the user is installing gp_log_collector using command "./gp_log_collector -i -t /tmp/gp_log_collect -l /tmp/gp_log_collect" from install path /tmp/gplogcollector_install
        Then bin/gp_log_collector and sbin/nodecollector.py should exist and have a new mtime
        And the directory /tmp/gp_log_collect exists
        And pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed or does not exist


    @mirror
    Scenario: Run gp_log_collector and collect mirror logs
        Given the database is running
        And the directory /tmp/gp_log_collect is removed or does not exist
        When the user runs "gp_log_collector -t /tmp/gp_log_collect -l /tmp/gp_log_collect -m"
        Then gp_log_collector should return a return code of 0
        And the directory /tmp/gp_log_collect exists
        And -m-pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed or does not exist
