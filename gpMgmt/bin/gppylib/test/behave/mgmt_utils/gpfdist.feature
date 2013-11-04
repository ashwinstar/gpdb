@gpfdist
Feature: gpfdist configure timeout value

    @smoke
    @wip
    Scenario: Validate timeout command line option - debug case 1
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_client.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        When the user runs "gpfdist -p 8080 -V -t 5 -l ./gpfdist.log &" 
        And the performance timer is started
        And the user runs client program "gpfdist_client.py" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Then the performance timer should be less then "7" seconds
        And the client program should print TIMEOUT PERIOD: to stdout with value in range 4 to 6
        And the client program should print HTTP/1.0 408 time out to stdout with value in range 4 to 6
        And the file "./gpfdist.log" is removed from the system
        And the "gpfdist" process is killed

    @fire
    @wip
    Scenario: Validate timeout command line option - debug case 2
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_client.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        When the user runs "gpfdist -p 8080 -V -t 30 -l ./gpfdist.log &" 
        And the performance timer is started
        And the user runs client program "gpfdist_client.py" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Then the performance timer should be less then "33" seconds
        And the client program should print TIMEOUT PERIOD: to stdout with value in range 29 to 31
        And the file "./gpfdist.log" is removed from the system
        And the "gpfdist" process is killed

    Scenario: Validate timeout command line option - debug case 3
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_client.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        When the user runs "gpfdist -p 8080 -V -t 61 -l ./gpfdist.log &" 
        And the performance timer is started
        And the user runs client program "gpfdist_client.py" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Then the performance timer should be less then "64" seconds
        And the client program should print TIMEOUT PERIOD: to stdout with value in range 60 to 62
        And the file "./gpfdist.log" is removed from the system
        And the "gpfdist" process is killed

    @fire
    Scenario: Validate timeout command line option - debug case 4
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_client.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        When the user runs "gpfdist -p 8080 -V -t 600 -l ./gpfdist.log &" 
        And the performance timer is started
        And the user runs client program "gpfdist_client.py" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Then the performance timer should be less then "605" seconds
        And the client program should print TIMEOUT PERIOD: to stdout with value in range 600 to 601
        And the file "./gpfdist.log" is removed from the system
        And the "gpfdist" process is killed

    @fire
    Scenario: Validate timeout command line option - debug case 5
        When the user runs "gpfdist -p 8080 -V -t 601" 
        Then the client program should print Error: -t timeout must be between 2 and 600, or 0 for no timeout error message
        And gpfdist should return a return code of 1 

    @fire
    Scenario: Validate timeout command line option - debug case 5
        When the user runs "gpfdist -p 8080 -V -t 5 -z 15" 
        Then the client program should print Error: -z listen queue size must be between 16 and 512 \(default is 256\) error message
        And gpfdist should return a return code of 1 
        When the user runs "gpfdist -p 8080 -V -t 5 -z 513" 
        Then the client program should print Error: -z listen queue size must be between 16 and 512 \(default is 256\) error message
        And gpfdist should return a return code of 1 

    @fire
    @wip
    Scenario: gpfdist select from simple external table
        Given the database is running
        And database "gpfdistdb" is created if not exists
        And the external table "read_ext_data" does not exist in "gpfdistdb"
        And the "gpfdist" process is killed
        Given the directory "extdata" exists in current working directory
        When the data line "Yandong|123" is appened to "extdata/data.txt" in cwd
        And the data line "Ivan|456" is appened to "extdata/data.txt" in cwd
        When the user runs "gpfdist -p 8080 -V -l ./gpfdist.log -d extdata &" 
        And a "READABLE" external table "read_ext_data" is created on file "data.txt" in "gpfdistdb" 
        And all rows from table "read_ext_data" db "gpfdistdb" are stored in the context
        Then validate that stored rows has "2" lines of output
        Then validate that "Yandong|123" "string|int" seperated by "|" is in the stored rows 
        Then validate that "Ivan|456" "string|int" seperated by "|" is in the stored rows 
        When the "gpfdist" process is killed
        Given the directory "extdata" does not exist in current working directory

    @wip
    Scenario: simple writable external table test
        Given the database is running
        And database "gpfdistdb" is created if not exists
        And the external table "write_ext_data" does not exist in "gpfdistdb"
        And the "gpfdist" process is killed
        Given the directory "extdata" exists in current working directory
        When a "WRITABLE" external table "write_ext_data" is created on file "write_data.txt" in "gpfdistdb"
        And the user runs "gpfdist -p 8080 -V -l ./gpfdist.log -d extdata &" 
        Given the row "'Ivan', 123" is inserted into "write_ext_data" in "gpfdistdb"
        And the row "'Yandong', 456" is inserted into "write_ext_data" in "gpfdistdb"
        Then the data line "Yandong|456" is appened to "extdata/write_data.txt" in cwd
        And the data line "Ivan|123" is appened to "extdata/write_data.txt" in cwd
        When the "gpfdist" process is killed
        Given the directory "extdata" does not exist in current working directory
