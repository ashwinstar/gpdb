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
    Scenario: Segment single nic failure
        Given eth1 on the first segment host is down
        And the database is running
        When the user runs "gp_log_collector -t /tmp/gp_log_collect -l /tmp/gp_log_collect"
        Then gp_log_collector should return a return code of 0
        And eth1 on the first segment host is up

    @dca
    Scenario: Verify and Clean up the collected log file after nic failure
        Given the user ran gp_log_collector and directory /tmp/gp_log_collect exists
        Then pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed

    Scenario: Handle log_dir creation failures
        When the user runs "gp_log_collector -l /dev/null/baddirectory"
        then gp_log_collector should print Failed to create or set permissions on to stdout

    Scenario: Handle tempDir creation failures on nodecollector
        When the user runs "gp_log_collector -t /tmp/gp_log_collect -t /dev/null/baddirectory"
        then gp_log_collector should print Not a directory to stdout

    @install
    Scenario: Install and run gp_log_collector
        Given the database is running
        When the user is installing gp_log_collector using command "./gp_log_collector -i -t /tmp/gp_log_collect -l /tmp/gp_log_collect" from install path /tmp/gplogcollector_install
        Then bin/gp_log_collector and sbin/nodecollector.py should exist and have a new mtime

    @install
    Scenario: Verify and Clean up the collected log file after install run
        Given the user ran gp_log_collector and directory /tmp/gp_log_collect exists
        Then pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed


    @mirror
    Scenario: Run gp_log_collector and collect mirror logs
        Given the database is running
        When the user runs "gp_log_collector -t /tmp/gp_log_collect -l /tmp/gp_log_collect -m"
        Then gp_log_collector should return a return code of 0

    @mirror
    Scenario:  The user performed a mirror log collection
	Given the user ran gp_log_collector and directory /tmp/gp_log_collect exists
        Then -m-pg_hba.conf should be found in tarball with prefix "GP_LOG_COLLECTION_" within directory /tmp/gp_log_collect
        And the directory /tmp/gp_log_collect is removed
