DROP TABLE IF EXISTS test_mat;
CREATE TABLE test_mat (i1 int, i2 int, i3 int, i4 int, i5 int, i6 int, i7 int, i8 int); 
INSERT INTO test_mat SELECT i,i,i,i,i,i,i,i FROM generate_series (0,29999) i; 
-- you need about 15,000 rows per segment to generate spill files for the first query below

SET statement_mem=1024;
set gp_resqueue_print_operator_memory_limits=on;
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set gp_workfile_type_hashjoin=bfz; 
set enable_hashjoin = false; 
set enable_nestloop = true; 

explain analyze SELECT t1.* FROM test_mat as t1, test_mat AS t2
WHERE t1.i1=t2.i2;
