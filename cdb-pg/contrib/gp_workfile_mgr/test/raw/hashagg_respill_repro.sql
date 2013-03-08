
-- A query causing respill in HashAgg
drop table if exists testhagg_big; 
create table testhagg_big (i1 int, i2 int, i3 int, i4 int);
insert into testhagg_big select i,i % 100000,i % 1000000,i from generate_series(0,4999999) i;

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem="4MB";
explain analyze select sum(i1) from testhagg_big group by i4;

