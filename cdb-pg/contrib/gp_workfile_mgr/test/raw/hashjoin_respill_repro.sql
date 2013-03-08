DROP TABLE IF EXISTS test1_big;
CREATE TABLE test1_big (i1 int, i2 int, i3 int, i4 int, i5 int, i6 int, i7 int, i8 int); 
INSERT INTO test1_big SELECT i,i,i,i,i,i,i,i FROM generate_series (0,499999) i; 
analyze test1_big; 

-- Insert more elements but don't update stats, so that we make HashJoin underestimate and respill
set gp_autostats_mode=none;
INSERT INTO test1_big SELECT i,i,i,i,i,i,i,i FROM generate_series (500000,4999999) i; 


SET gp_workfile_caching=true;
SET statement_mem='16MB';
SET gp_workfile_caching_loglevel="LOG"; 
set gp_workfile_type_hashjoin=buffile; 

explain analyze SELECT t1.* FROM test1_big AS t1, test1_big AS t2 WHERE t1.i1=t2.i2;
