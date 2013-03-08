-- http://fisheye.greenplum.com/changelog/cdb2-main?cs=25289
-- http://jira.greenplum.com/browse/MPP-2819

drop table if exists huge_tuples;
drop table if exists large_tuples;

create table large_tuples(acamp_id int, dw_snapshot_dttm timestamp without time zone, expression text, dw_snapshot_dt date, dw_source_file_schema_version text) distributed randomly;

copy large_tuples (acamp_id, dw_snapshot_dttm, expression, dw_snapshot_dt, dw_source_file_schema_version)
from '/Users/caragg/dev/projects/spill_file_reuse/subtasks/materialize/lob_table.dat' with delimiter '|';

create table huge_tuples (acamp_id int, dw_snapshot_dttm timestamp without time zone, expression text, expression2 text, dw_snapshot_dt date, dw_source_file_schema_version text) distributed randomly;

insert into huge_tuples
  select 
  acamp_id, 
  dw_snapshot_dttm, 
  repeat(expression,3) as expression, 
  expression as expression2, dw_snapshot_dt, 
  dw_source_file_schema_version
from large_tuples; 

explain analyze select count(*)
from ( SELECT acamp_id,
dw_snapshot_dttm,
expression ,
LAG(expression,1) OVER (PARTITION BY acamp_id ORDER BY dw_snapshot_dttm) AS prior_expression
from huge_tuples ) foo
where acamp_id is null ;

--set enable_hashjoin = false; 
--set enable_nestloop = true; 

-- Query that fails
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
insert into huge_tuples select * from huge_tuples; 
-- This should result in a table with 86016 rows

explain analyze select count(*)
from ( SELECT acamp_id,
dw_snapshot_dttm,
expression ,
LAG(expression,1) OVER (PARTITION BY acamp_id ORDER BY dw_snapshot_dttm) AS prior_expression
from huge_tuples ) foo
where acamp_id is null ;
