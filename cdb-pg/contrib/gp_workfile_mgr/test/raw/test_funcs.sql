reset statement_mem;

drop function if exists  f1_filter_rows(); 
drop function if exists  f2_filter_rows(); 
drop table if exists testfunc1; 
create table testfunc1 (i1 int, i2 int, i3 int); 
insert into testfunc1 select i,i,i from generate_series(0,999999) i;


CREATE OR REPLACE FUNCTION f1_filter_rows() RETURNS setof testfunc1 
AS $$
   SELECT * FROM testfunc1 where i1 % 2 = 0 order by i2
$$ LANGUAGE SQL;

CREATE OR REPLACE FUNCTION f2_filter_rows() RETURNS setof testfunc1 
AS $$
DECLARE r
	testfunc1%rowtype; 
BEGIN
   FOR r in SELECT * FROM testfunc1 where i1 % 2 = 0 order by i2
LOOP
RETURN NEXT r; 
END LOOP;
RETURN;
END
$$ LANGUAGE PLPGSQL;

SET gp_workfile_caching=true; 
SET gp_workfile_caching_loglevel="LOG"; 
set statement_mem=10240;
set gp_resqueue_print_operator_memory_limits=on;


/* Query with function call */ 
explain 
analyze
select max(i1) from (
       select * from f2_filter_rows()
) foo
group by i2; 
