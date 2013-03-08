drop table if exists csq_t1;
drop table if exists csq_t2;
drop table if exists csq_t3;
drop table if exists csq_t3_small; 

create table csq_t1(a int, b int);
insert into csq_t1 values (1,2);
insert into csq_t1 values (3,4);
insert into csq_t1 values (5,6);
insert into csq_t1 values (7,8);

create table csq_t2(x int,y int);
insert into csq_t2 values(1,1);
insert into csq_t2 values(3,9);
insert into csq_t2 values(5,25);
insert into csq_t2 values(7,49);

create table csq_t3_small(c int, d text);
insert into csq_t3_small values(1,'one');
insert into csq_t3_small values(3,'three');
insert into csq_t3_small values(5,'five');
insert into csq_t3_small values(7,'seven');

create table csq_t3(c int, d int);
insert into csq_t3 select i, i % 100000 from generate_series(0,999999) i; 

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set gp_resqueue_print_operator_memory_limits=on;

set gp_enable_multiphase_agg=off;
set statement_mem=5000;

-- RESCAN HASHAGG, MATERIALIZE --
explain analyze
select a, 
       (select y 
        from (select * 
	      from csq_t2, 
	      	   (select d from csq_t3 group by d) tt2
	      where csq_t2.y = tt2.d) tt 
	where x=a) 
from csq_t1 
;

set statement_mem=10240;

-- RESCAN SORT, MATERIALIZE -- 
explain analyze
select a, 
       (select y 
        from (select * 
	      from csq_t2, 
	      	   (select c from csq_t3 order by d) tt2
	      where csq_t2.y = tt2.c) tt 
	where x=a) 
from csq_t1 
;

