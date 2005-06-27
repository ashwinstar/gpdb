Smoke tests read_me:

written June 27, 2005
by: Joan Hoenow

Purpose of the tests:
-these are quick, self-contained tests that execute over 200 queries from the cdb-unit test suite.

-these will probably be obsolete when cdb-unit can be run easily by developers

-these would typically be executed as part of a procedure that set up a database first.

Example of an execution on a system with cdb2 running:

cdbstop
cdbclean
cdbinitdb -n 2
cdbstart -u
cdbcreatedb -s smoke

--note that you would not run cdbpopulate. the cdbpopulate command will create the schema and
--insert data, but your results from the queries may not reliably match the expected output
--CAUTION: avoid using cdbpopulate.

psql smoke -a -f smoke_schema.sql  &> smoke_schema.results
psql smoke -a -f smoke_data.sql    &> smoke_data.results
psql smoke -a -f smoke_queries.sql &> smoke_ouput_test01.results

diff smoke_output_test01.results smoke_output.results

--note that your output may vary with the sort order of the queries.
--most of the queries do not have order-by clause.
--if needed, sort the results files first and then compare.
--this will not detect errors in the ORDER BY processing. if you are working on that functoin,
--you will need the features of cdbunit to adequately test.

--the -a option is used to echo the SQL statements in the output.

--this is not a complete test set.
--tests for bad syntax have been excluded.
--tests which currently fail for cdbunit have been exclued (most set operations and some queries
	which cannot be parallelized).
