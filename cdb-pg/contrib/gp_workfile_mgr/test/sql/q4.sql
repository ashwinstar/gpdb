---- Q4 ---
-- Tests HashAgg, materialize and sort with spilling.

CREATE TEMPORARY TABLE csq_t1(a int, b int);
INSERT INTO csq_t1 VALUES (1,2);
INSERT INTO csq_t1 VALUES (3,4);
INSERT INTO csq_t1 VALUES (5,6);
INSERT INTO csq_t1 VALUES (7,8);

CREATE TEMPORARY TABLE csq_t2(x int,y int);
INSERT INTO csq_t2 VALUES(1,1);
INSERT INTO csq_t2 VALUES(3,9);
INSERT INTO csq_t2 VALUES(5,25);
INSERT INTO csq_t2 VALUES(7,49);

CREATE TEMPORARY TABLE csq_t3_small(c int, d text);
INSERT INTO csq_t3_small VALUES(1,'one');
INSERT INTO csq_t3_small VALUES(3,'three');
INSERT INTO csq_t3_small VALUES(5,'five');
INSERT INTO csq_t3_small VALUES(7,'seven');

CREATE TEMPORARY TABLE csq_t3(c int, d int);
INSERT INTO csq_t3 SELECT i, i % 100000 FROM generate_series(0,999999) i; 

-- Set parameters --
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
SET gp_resqueue_print_operator_memory_limits=on;
SET gp_enable_multiphase_agg=off;
SET statement_mem=5000;

-- RESCAN HASHAGG, MATERIALIZE --
SELECT a, 
       (SELECT y 
        FROM (SELECT * 
	      FROM csq_t2, 
	      	   (SELECT d FROM csq_t3 GROUP by d) tt2
	      WHERE csq_t2.y = tt2.d) tt 
	WHERE x=a) 
FROM csq_t1;

-- Set parameters --
SET statement_mem=10240;

-- RESCAN SORT, MATERIALIZE -- 
SELECT a, 
       (SELECT y 
        FROM (SELECT * 
	      FROM csq_t2, 
	      	   (SELECT c FROM csq_t3 ORDER BY d) tt2
	      WHERE csq_t2.y = tt2.c) tt 
	WHERE x=a) 
FROM csq_t1;

