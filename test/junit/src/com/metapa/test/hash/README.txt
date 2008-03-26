===============================================================================
| CDB-HASH Unit Tests
|
| Author: Alon Goldshuv
| Version: $Revision$
===============================================================================

Before running the tests:
-------------------------

1. Make sure you have CDB installed.
2. CDB and libcdbtest needs to be installed!
2. Start the database CDB.
4. Edit the test.properties file to include your cluster's information and test information.

How to run the tests:
---------------------

ant cdbhash

After running the tests:
------------------------

a new directory called "junit_report" is created in the test
directory level. This directory will include the results of each
tests case and some additional information on some occasions. In the
case of a test failure or error the detailed results of the failure
will be located there.



