This is feng's quick and dirty smoke test.  You need to set up your box so that:

0. The binaries are in your path.
1. mppstart.sh -a will successfully start the server.
2. setup a env. variable export 
	OLAPTEST_PORT=your_port (default 12000 if not set)  
	OLAPTEST_DB=yourdb (default olapsmokedb if not set)
3. In the olaptest directory, do ./runXXX.sh

XXX. the suite will drop create a database called $OLAPTEST_DB.  If you have such an db, it will be ...


So far the tests includes the following, 
1. the smoke queries in test dir,
2. some tpch query

XXX. The smoke test may fail due to order or rows returned.  We will need Jeff's "orderless diff".
	While for now, a really quick, dirty, not safe way is to do 
		grep rows result/smoke.*.diff
	There should be no result.

XXX. the postgres regress tests are not ready.  There are too many failures and I don't have time
 	to investigate whether is expected (as in negative test), or it is really a regress that should 
	be fixed.

