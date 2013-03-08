reset statement_mem; 
drop table if exists testsisc;
create table testsisc (i1 int, i2 int, i3 int, i4 int); 
insert into testsisc select i, i % 1000, i % 100000, i % 75 from generate_series(0,199999) i; 

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem="5MB";
set gp_resqueue_print_operator_memory_limits=on;

set gp_cte_sharing=on;

-- SiSc Xslice Mat
explain analyze
with ctesisc as 
  (select count(i1) as c1,i3 as c2 from testsisc group by i3)
select *
from ctesisc as t1, ctesisc as t2
where t1.c1 = t2.c2; 

