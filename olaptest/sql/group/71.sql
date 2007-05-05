drop table rt; --ignore
create table rt (a int, b int, c int);
insert into rt values (1,1,1),(1,2,1),(1,2,1),(2,1,1),(2,2,1);
select * from rt;
select a,b,sum(c) from rt group by rollup(a,b);
insert into rt values (1,null,1);
select a,b,sum(c) from rt group by rollup(a,b);
insert into rt values (null,null,1);
select a,b,sum(c) from rt group by rollup(a,b);
drop table rt; --ignore
