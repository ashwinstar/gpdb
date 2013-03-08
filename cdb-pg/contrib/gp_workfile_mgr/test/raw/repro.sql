
SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem=5000;
set gp_resqueue_print_operator_memory_limits=on;

explain analyze
select count(i1) from generate_series(0,499) i1, generate_series(100,199) i2 group by i1,i2;

