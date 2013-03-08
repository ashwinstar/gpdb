
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem=10240;
set gp_resqueue_print_operator_memory_limits=on;

/* Query with function call */ 
explain 
analyze
select max(i1) from (
       select i as i1,i as i2,i as i3 from generate_series(0,999999) i
) foo
group by i2; 

/* Query with subquery and function call */ 
explain 
analyze
select max(i1) from (
       select i as i1,i as i2,i as i3 from generate_series(0,999999) i
       union 
       select i as i1,i as i2,i as i3 from generate_series(0,99) i
) foo
group by i2; 


/* Query with subquery and function call and values */ 
explain 
analyze
select max(i1) from (
       select i as i1,i as i2,i as i3 from generate_series(0,999999) i
       union 
       values ( 1, 2, 3), (4, 5, 6), (7, 8, 9)
) foo
group by i2; 


SET statement_mem=2048;
explain
analyze 
select max(i1) from (
   SELECT t1.* FROM test1 AS t1, test1 AS t2 
   WHERE t1.i1=t2.i2
) foo
group by i2; 
