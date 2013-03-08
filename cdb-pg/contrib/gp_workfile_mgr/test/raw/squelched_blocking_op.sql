drop table if exists testsquelch1;
create table testsquelch1 (i int, j int);

drop table if exists testsquelch2;
create table testsquelch2 (i int, j int);
insert into testsquelch2 select i, i from generate_series(0, 999999) i;

set statement_mem=1024;
explain analyze select * from testsquelch1, (select i, (rank() over(order by j))::int as rank from testsquelch2) tmp where testsquelch1.j = tmp.rank;
