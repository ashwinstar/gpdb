---- Q3 ---
-- Tests HashAgg Operator that spills.

CREATE TEMPORARY TABLE testhagg (i1 int, i2 int, i3 int, i4 int);
INSERT INTO testhagg SELECT i,i,i,i from generate_series(0,999999) i;

-- Set parameters --
SET gp_workfile_caching=true; 
SET statement_mem=10240;
SET gp_resqueue_print_operator_memory_limits=on;

SELECT MAX(i1) 
FROM testhagg 
GROUP BY i2;
