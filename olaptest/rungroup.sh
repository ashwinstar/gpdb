#!/bin/bash

# first, start the server
# mppstart.sh -a

OLAPTEST_PORT=${OLAPTEST_PORT:-12000}
OLAPTEST_DB=${OLAPTEST_DB:-olapsmokedb}
FAILEDTEST=0

# clean up results first
rm -f result/group*

# create a database
echo "Drop database olapsmokedb ... "
psql -p $OLAPTEST_PORT template1 -c "drop database ${OLAPTEST_DB};"
echo "Creating database olapsmokedb ... "
psql -p $OLAPTEST_PORT template1 -c "create database ${OLAPTEST_DB};"

echo
echo "Running groupping queries ... "
TESTSUITE=group
for TESTCASE in cvpschema cvpdata 
do
	echo "Running group test $TESTCASE ... "
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

for ((n=1; n <= 81; n++))
do
	echo "Running group test $n ... "
	psql -p $OLAPTEST_PORT -d $OLAPTEST_DB -f sql/$TESTSUITE/${n}.sql -o result/${TESTSUITE}.${n}.out

	diff expected/${TESTSUITE}.${n}.exp result/${TESTSUITE}.${n}.out > result/${TESTSUITE}.${n}.diff
	if [ $? -eq 0 ]
	then
		echo "Test $n passed."
	else
		let "FAILEDTEST += 1"
		echo "Test $n failed.  See result/${TESTSUITE}.${n}.diff "
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


