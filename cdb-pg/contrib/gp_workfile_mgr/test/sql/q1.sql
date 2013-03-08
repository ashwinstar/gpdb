---- Q1 ---
-- Tests Hash Join that spills.

CREATE TEMPORARY TABLE t (i1 int, i2 int, i3 int, i4 int, i5 int, i6 int, i7 int, i8 int); 
INSERT INTO t SELECT i,i,i,i,i,i,i,i FROM generate_series (0,79999) i; 

-- Set parameters --
SET statement_mem=2048;
SET gp_resqueue_print_operator_memory_limits=on;
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
SET gp_workfile_type_hashjoin=bfz; 

SELECT t1.* 
FROM t AS t1, t AS t2	
WHERE t1.i1=t2.i2;
