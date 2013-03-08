
-- This is from MPP-15087, query that rescans and eager-frees materialize
create table t(code char(3), n numeric);
insert into t values ('abc',1);
insert into t values ('xyz',2);  
insert into t values ('def',3); 

with cte as 
	(
	select code, n, x 
	from t 
	, (select 100 as x) d 
	)
select code from t where 1= (select count(*) from cte where cte.code::text=t.code::text or cte.code::text = t.code::text);

-- This is similar query, but causes spilling. For a 2P/2M configuration, this spills on segment 1 (not 0). Look there. 

create table tbig(code char(1000), n numeric);
insert into tbig select repeat((i%10)::char, 1000), i from generate_series(0,999) i;

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set gp_resqueue_print_operator_memory_limits=on;
set statement_mem='2MB';

explain analyze
with cte as 
	(
	select code, n, x 
	from tbig 
	, (select 100 as x) d 
	)
select code from tbig where 1= (select count(*) from cte where cte.code::text=tbig.code::text or cte.code::text = tbig.code::text);

--- Trying to make inner materialize spill -- 
create table t2(code char(10000), n numeric);
insert into t2 select repeat((i%10)::char, 10000), i from generate_series(0,99999) i;

explain 
--analyze
with cte as 
	(
	select code, n, x 
	from t2 
	, (select 100 as x) d 
	)
select code from tbig where 1= (select count(*) from cte where cte.code::text=tbig.code::text or cte.code::text = tbig.code::text);
