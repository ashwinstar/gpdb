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

    @gpfdist_wait_before_close
    Scenario: gpfdist wait some time before close file - simple 
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_httpclient.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        Given the directory "extdata" exists in current working directory
        When the user runs "gpfdist -p 8080 -V -l ./gpfdist.log -d extdata -w 2 & echo $! > extdata/gpfdist.pid &" 
        And the data line "abcd" is appened to "extdata/data.txt" in cwd

        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=0" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=0 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Given waiting "1" seconds
        Then the file "./extdata/data.txt" by process "extdata/gpfdist.pid" is not closed 
        
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Given waiting "3" seconds
        Then the file "extdata/data.txt" by process "extdata/gpfdist.pid" is closed 
        When the "gpfdist" process is killed
        Given the directory "extdata" does not exist in current working directory

    @gpfdist_wait_before_close
    Scenario: gpfdist wait some time before close file - multi session 
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_httpclient.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        Given the directory "extdata" exists in current working directory
        When the user runs "gpfdist -p 8080 -V -l ./gpfdist.log -d extdata -w 3 & echo $! > extdata/gpfdist.pid &" 
        And the data line "abcd" is appened to "extdata/data.txt" in cwd

        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=0 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=0 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        Given waiting "1" seconds
        Then the file "./extdata/data.txt" by process "extdata/gpfdist.pid" is not closed 
        
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=1 --session_id=2" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=2 --segment_index=1 --session_id=2 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD

        Then the file "./extdata/data.txt" by process "extdata/gpfdist.pid" opened number is "2"
        
        Given waiting "4" seconds
        Then the file "extdata/data.txt" by process "extdata/gpfdist.pid" is closed 
        When the "gpfdist" process is killed
        Given the directory "extdata" does not exist in current working directory

    @gpfdist_wait_before_close
    Scenario: gpfdist wait some time before close file - more segments
        Given the "gpfdist" process is killed
        Given the client program "gpfdist_httpclient.py" is present under CWD in "gppylib/test/behave_utils/gpfdist_utils"
        Given the directory "extdata" exists in current working directory
        When the user runs "gpfdist -p 8080 -V -l ./gpfdist.log -d extdata -w 3 & echo $! > extdata/gpfdist.pid &" 
        And the data line "abcd" is appened to "extdata/data.txt" in cwd

        # seg0 send data
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=0 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg1 send data
        And the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=1 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg1 send done
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=1 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg0 send done
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=0 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD

        Given waiting "1" seconds
        Then the file "./extdata/data.txt" by process "extdata/gpfdist.pid" is not closed 
        
        # seg2 send data
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=2 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg3 send data
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=3 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg2 send done
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=2 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg3 send done
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=3 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD

        Given waiting "1" seconds
        Then the file "./extdata/data.txt" by process "extdata/gpfdist.pid" is not closed 
        
        # seg4 send data
        When the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=4 --session_id=1" from "gppylib/test/behave_utils/gpfdist_utils" under CWD
        # seg4 send done
        And  the user runs client program "gpfdist_httpclient.py --url=localhost:8080 --path=/data.txt --segment_count=8 --segment_index=4 --session_id=1 --isdone" from "gppylib/test/behave_utils/gpfdist_utils" under CWD

        Given waiting "4" seconds
        Then the file "extdata/data.txt" by process "extdata/gpfdist.pid" is closed 
        When the "gpfdist" process is killed
        Given the directory "extdata" does not exist in current working directory



