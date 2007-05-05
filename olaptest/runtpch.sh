#!/bin/bash

# first, start the server
# mppstart.sh -a

OLAPTEST_PORT=${OLAPTEST_PORT:-12000}
OLAPTEST_DB=${OLAPTEST_DB:-olapsmokedb}
FAILEDTEST=0

# clean up results first
rm -f result/tpch*

# create copytiny.sql
./gencopytiny.sh

# create a database
echo "Drop database olapsmokedb ... "
psql -p $OLAPTEST_PORT template1 -c "drop database ${OLAPTEST_DB};"
echo "Creating database olapsmokedb ... "
psql -p $OLAPTEST_PORT template1 -c "create database ${OLAPTEST_DB};"

echo
echo "Running smoke queries ... "
TESTSUITE=tpch
for TESTCASE in create copytiny 1 3 4 5 6 7 8 9 10 11 12 13 14 15 18 19 
do
	echo "Running tpch test $TESTCASE ... "
	psql -p $OLAPTEST_PORT -d $OLAPTEST_DB -f sql/$TESTSUITE/${TESTCASE}.sql -o result/${TESTSUITE}.${TESTCASE}.out 

	diff expected/${TESTSUITE}.${TESTCASE}.exp result/${TESTSUITE}.${TESTCASE}.out > result/${TESTSUITE}.${TESTCASE}.diff
	if [ $? -eq 0 ]
	then
		echo "Test $TESTCASE passed."
	else
		let "FAILEDTEST += 1"
		echo "Test $TESTCASE failed.  See result/${TESTSUITE}.${TESTCASE}.diff "
	fi
done

echo
echo
if [ $FAILEDTEST -eq 0 ]
then
	echo "All test passed!"
else
	echo "There are $FAILEDTEST failures.  See result/*.diff "
	echo "Some failures are due to order, but at least the following are real hard failures "
	grep -i 'row' result/${TESTSUITE}.*.diff
fi


