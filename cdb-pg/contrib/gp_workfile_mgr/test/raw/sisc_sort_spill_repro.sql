drop table if exists testsisc;
create table testsisc (i1 int, i2 int, i3 int, i4 int); 
insert into testsisc select i, i % 1000, i % 100000, i % 75 from generate_series(0,199999) i; 

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem="5MB";
set gp_resqueue_print_operator_memory_limits=on;

set gp_enable_mk_sort=on;
set gp_cte_sharing=on;

-- SiSc Xslice Sort
explain analyze
with ctesisc as 
  (select * from testsisc order by i2)
select *
from ctesisc as t1, ctesisc as t2
where t1.i1 = t2.i2; 

-- This one is SiSc intra-slice
explain 
select row_number() over(partition by i1 order by i2), 
       count(*) over (partition by i1) 
from testsisc;
