---- Q2 ---
-- Tests Sort Operator that spills.

CREATE TEMPORARY TABLE testsort (i1 int, i2 int, i3 int, i4 int); 
INSERT INTO testsort SELECT i, i % 1000, i % 100000, i % 75 FROM generate_series(0,199999) i; 

-- Set parameters --
SET gp_workfile_caching=true; 
SET statement_mem="5MB";
SET gp_resqueue_print_operator_memory_limits=on;

SELECT i1,i2 
FROM testsort 
ORDER BY i2;
