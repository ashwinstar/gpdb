===============================================================================
| CDB-Loader Unit Tests
|
| Author: Alon Goldshuv
| Version: $Revision$
===============================================================================

The following java test cases (in test/com.metapa.cdb.loader) make a set of unit
tests for the cdb-loader source (in src/com.metapa.cdb.loader).

Before running the tests:
-------------------------

1. Make sure that you have an existing CDB cluster.
2. Start CDB.
3. Edit the test.properties file to include your cluster's information.

How to run the tests:
---------------------

(from <projectroot>/cdb-loader)

    ant runTests

After running the tests:
------------------------

a new directory called "junit_report" is created in the cdb-loader
directory level. This directory will include the results of each
tests case and some additional information on some occasions. In the
case of a test failure or error the detailed results of the failure
will be located there.




