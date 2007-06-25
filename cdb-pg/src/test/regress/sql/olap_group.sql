--
-- OLAP_GROUP - Test OLAP GROUP BY extensions
--

-- Syntactic equivalents --

--begin equivalent
select count(*) from sale;
select count(*) from sale group by ();
--end equivalent

--begin equivalent
select cn, count(*) from sale group by cn;
select cn, count(*) from sale group by (), cn;
select cn, count(*) from sale group by cn, ();
--end equivalent

--begin equivalent
select cn, vn, count(*) from sale group by cn, vn;
select cn, vn, count(*) from sale group by (), cn, vn;
select cn, vn, count(*) from sale group by cn, (), vn;
select cn, vn, count(*) from sale group by cn, vn, ();
select cn, vn, count(*) from sale group by (), cn, (), vn, ();
--end equivalent


-- Semantic equivalents --

--begin equivalent
select cn, vn, pn, sum(qty*prc) from sale group by cn, vn, pn
union all
select cn, vn, null, sum(qty*prc) from sale group by cn, vn
union all
select cn, null, null, sum(qty*prc) from sale group by cn
union all
select null, null, null, sum(qty*prc) from sale;    
select cn, vn, pn, sum(qty*prc) from sale group by rollup(cn,vn,pn);
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn));
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets((cn,vn), (), (cn,vn,pn), (cn));
--end equivalent

--begin equivalent
select cn, vn, pn, sum(qty*prc) from sale group by cn, vn, pn
union all
select cn, vn, null, sum(qty*prc) from sale group by cn, vn
union all
select cn, null, null, sum(qty*prc) from sale group by cn
union all
select null, null, null, sum(qty*prc) from sale
union all
select cn, null, pn, sum(qty*prc) from sale group by cn, pn
union all
select null, vn, pn, sum(qty*prc) from sale group by vn, pn
union all
select null, vn, null, sum(qty*prc) from sale group by vn
union all
select null, null, pn, sum(qty*prc) from sale group by pn;
select cn, vn, pn, sum(qty*prc) from sale group by cube (cn, vn, pn);
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn));
--end equivalent

-- begin equivalent
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn
union all
select cn, null, null, count(distinct dt) from sale group by cn
union all
select null, null, null, count(distinct dt) from sale;    
select cn, vn, pn, count(distinct dt) from sale group by rollup(cn,vn,pn);
select cn, vn, pn, count(distinct dt) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn));
-- end equivalent

--begin equivalent order 1,2,3
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn
union all
select cn, null, null, count(distinct dt) from sale group by cn
union all
select null, null, null, count(distinct dt) from sale
union all
select cn, null, pn, count(distinct dt) from sale group by cn, pn
union all
select null, vn, pn, count(distinct dt) from sale group by vn, pn
union all
select null, vn, null, count(distinct dt) from sale group by vn
union all
select null, null, pn, count(distinct dt) from sale group by pn
order by 1,2,3; -- order 1,2,3
select cn, vn, pn, count(distinct dt) from sale group by cube (cn, vn, pn) order by 1,2,3; -- order 1,2,3
select cn, vn, pn, count(distinct dt) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3; -- order 1,2,3
--end equivalent

--begin equivalent order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by cn, vn, pn
union all
select cn, vn, null, sum(qty*prc) from sale group by cn, vn
union all
select cn, null, null, sum(qty*prc) from sale group by cn
union all
select null, null, null, sum(qty*prc) from sale 
order by 1,2,3; -- order 1,2,3    
select cn, vn, pn, sum(qty*prc) from sale group by rollup(cn,vn,pn) order by 1,2,3; -- order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by 1,2,3; -- order 1,2,3
--end equivalent

--begin equivalent order 3,4,1
select pn, sum(qty*prc), cn, vn from sale group by cn, vn, pn
union all
select null, sum(qty*prc), cn, vn from sale group by cn, vn
union all
select null, sum(qty*prc), cn, null from sale group by cn
union all
select null, sum(qty*prc), null, null from sale 
order by cn, vn, pn; -- order 3,4,1
select pn, sum(qty*prc), cn, vn from sale group by rollup(cn,vn,pn) order by cn, vn, pn; -- order 3,4,1
select pn, sum(qty*prc), cn, vn from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by cn, vn, pn; -- order 3,4,1
--end equivalent

--begin equivalent order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by cn, vn, pn
union all
select cn, vn, null, sum(qty*prc) from sale group by cn, vn
union all
select cn, null, null, sum(qty*prc) from sale group by cn
union all
select null, null, null, sum(qty*prc) from sale
union all
select cn, null, pn, sum(qty*prc) from sale group by cn, pn
union all
select null, vn, pn, sum(qty*prc) from sale group by vn, pn
union all
select null, vn, null, sum(qty*prc) from sale group by vn
union all
select null, null, pn, sum(qty*prc) from sale group by pn 
order by 1,2,3; -- order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by cube (cn, vn, pn) order by 1,2,3; -- order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3; -- order 1,2,3
--end equivalent

-- ***BUG*** The extended groupings aren't correctly ordered! Maybe they wrongly parallel sorted!
--begin equivalent order 1,2,3
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn
union all
select cn, null, null, count(distinct dt) from sale group by cn
union all
select null, null, null, count(distinct dt) from sale 
order by 1,2,3; -- order 1,2,3  
select cn, vn, pn, count(distinct dt) from sale group by rollup(cn,vn,pn) order by 1,2,3; -- order 1,2,3  
select cn, vn, pn, count(distinct dt) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by 1,2,3; -- order 1,2,3  
--end equivalent

--begin equivalent order 1,2,3
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn
union all
select cn, null, null, count(distinct dt) from sale group by cn
union all
select null, null, null, count(distinct dt) from sale
union all
select cn, null, pn, count(distinct dt) from sale group by cn, pn
union all
select null, vn, pn, count(distinct dt) from sale group by vn, pn
union all
select null, vn, null, count(distinct dt) from sale group by vn
union all
select null, null, pn, count(distinct dt) from sale group by pn 
order by 1,2,3; -- order 1,2,3  
select cn, vn, pn, count(distinct dt) from sale group by cube (cn, vn, pn) order by 1,2,3; -- order 1,2,3  
select cn, vn, pn, count(distinct dt) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3; -- order 1,2,3  
--end equivalent


-- Ordinary Grouping Set Specifications --

select cn, count(*) from sale group by cn;
select cn, count(*) from sale group by (cn);

select cn,pn,count(*) from sale group by cn,pn;
select cn,pn,count(*) from sale group by (cn),pn;
select cn,pn,count(*) from sale group by cn,(pn);
select cn,pn,count(*) from sale group by (cn),(pn);

select cn+vn as a, vn+pn as b, count(*) from sale group by (cn+vn), (vn+pn);
select cn+vn as a, vn+pn as b, count(*) from sale group by (1), (2);
select cn+vn as a, vn+pn as b, count(*) from sale group by (a), (b);
select cn+vn as a, vn+pn as b, count(*) from sale group by a, b;
select cn+vn as a, vn+pn as b, count(*) from sale group by (cn+vn, vn+pn);
select cn+vn as a, vn+pn as b, count(*) from sale group by (1, 2);
select cn+vn as a, vn+pn as b, count(*) from sale group by (a, b);

select cn,vn,pn,count(*) from sale group by cn,(vn,pn);

select count(*) from sale group by rollup((cn,vn),(pn,dt));


-- Distinguish grouping value NULLs from summarization NULLs --

create table olap_tmp (a int, b int, c int);

insert into olap_tmp values (1,1,1),(1,2,1),(1,2,1),(2,1,1),(2,2,1);

--begin equivalent
select * from olap_tmp;
select * from (values (1,1,1),(1,2,1),(1,2,1),(2,1,1),(2,2,1)) r(a,b,c);
--end equivalent

--begin equivalent
select a,b,sum(c) from olap_tmp group by rollup(a,b);
select * from (values (1,1,1), (1,2,2), (1,null,3), (2,1,1), (2,2,1),(2,null,2),(null,null,5)) r(a,b,"sum");
--end equivalent

insert into olap_tmp values (1,null,1);

--begin equivalent
select a,b,sum(c) from olap_tmp group by rollup(a,b);
select * from (values (1,1,1), (1,2,2), (1,null,1), (1,null,4), (2,1,1), (2,2,1), (2,null,2), (null,null,6)) r(a,b,"sum");
--end equivalent

insert into olap_tmp values (null,null,1);

--begin equivalent
select a,b,sum(c) from olap_tmp group by rollup(a,b);
select * from (values (1,1,1), (1,2,2), (1,null,1), (1,null,4), (2,1,1), (2,2,1), (2,null,2), (null,null,1), (null,null,1), (null,null,7)) r(a,b,"sum");
--end equivalent

drop table olap_tmp; --ignore

-- Grouping extension combination
--begin equivalent
select cn,vn,sum(qty) from sale group by cn,vn union all
select cn,null,sum(qty) from sale group by cn union all
select cn,null,sum(qty) from sale group by cn union all
select null,null,sum(qty) from sale;
select cn,vn,sum(qty) from sale group by grouping sets (rollup(cn,vn), cn);
--end equivalent

--begin equivalent
select cn,vn,sum(qty) from sale group by cn,vn union all
select cn,null,sum(qty) from sale group by cn union all
select null,null,sum(qty) from sale;
select cn,vn,sum(qty) from sale group by grouping sets (rollup(cn,vn));
--end equivalent

--begin equivalent
select cn,null as vn,sum(qty) from sale group by cn union all
select null as cn,vn,sum(qty) from sale group by vn;
select cn,vn,sum(qty) from sale group by grouping sets((cn),(vn));
--end equivalent

-- GROUPING function -- 

select grouping(cn,vn,pn) from sale;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by cn,vn,pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn),vn,pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn,vn), pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn);
select cn,vn,pn,grouping(cn,vn,pn) from sale group by cn, rollup(vn,pn);
select cn,vn,pn,grouping(cn,vn,pn) from sale group by vn, rollup(cn, pn);
select grouping(cn), grouping(vn), grouping(pn), cn, vn, pn, count(*) from sale group by rollup(cn,vn,pn);
select grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn) order by 1 desc; -- order 1
select grouping(cn), grouping(vn), grouping(pn) from sale group by rollup(cn,vn,pn) order by 1 desc, 2 desc, 3 desc; -- order 1,2,3
select cn+vn,pn, grouping(cn+vn,pn), count(*) from sale group by rollup(cn+vn,pn);

--begin equivalent
select cn,vn,0 as grouping from sale group by cn,vn
union all select cn,null,1 as grouping from sale group by cn
union all (select null,null,3 as grouping from sale limit 1)
union all select null,vn,2 as grouping from sale group by vn;
select cn,vn,grouping(cn,vn) from sale group by cube(cn,vn);
select cn,vn,grouping(cn,vn) from sale group by cube(vn,cn);
select cn,vn,grouping(cn,vn) from sale group by grouping sets ((vn,cn), (vn), (cn), ());
select cn,vn,grouping(cn,vn) from sale group by grouping sets ((),(vn,cn), (vn), (cn));
--end equivalent

--begin equivalent
select cn,vn,0 as grouping from sale group by cn,vn
union all select cn,null,2 as grouping from sale group by cn
union all (select null,null,3 as grouping from sale limit 1)
union all select null,vn,1 as grouping from sale group by vn;
select cn,vn,grouping(vn,cn) from sale group by cube(cn,vn);
select cn,vn,grouping(vn,cn) from sale group by cube(vn,cn);
select cn,vn,grouping(vn,cn) from sale group by grouping sets ((vn,cn), (vn), (cn), ());
select cn,vn,grouping(vn,cn) from sale group by grouping sets ((vn), (cn), (), (cn,vn));
--end equivalent

--begin equivalent
select cn,dt,0 as grouping from sale group by cn,dt
union all select cn,null,1 as grouping from sale group by cn
union all (select null,null,3 as grouping from sale limit 1)
union all select null,dt,2 as grouping from sale group by dt;
select cn,dt,grouping(cn,dt) from sale group by cube(cn,dt);
select cn,dt,grouping(cn,dt) from sale group by cube(dt,cn);
--end equivalent

--begin equivalent
select cn,dt,count(vn),0 as grouping from sale group by cn,dt
union all select cn,null,count(vn),1 as grouping from sale group by cn
union all (select null,null,count(vn),3 as grouping from sale limit 1)
union all select null,dt,count(vn),2 as grouping from sale group by dt;
select cn,dt,count(vn),grouping(cn,dt) from sale group by cube(cn,dt);
select cn,dt,count(vn),grouping(cn,dt) from sale group by cube(dt,cn);
--end equivalent

-- Ungrouped attributes in GROUPING function --

select grouping(cn,vn,pn) from sale group by cn; --error
select grouping(cn,vn,pn) from sale group by rollup(cn); --error

-- Using in View --

create view cube_view as select cn,vn,grouping(vn,cn) from sale group by cube(cn,vn);
\d cube_view;
create view rollup_view as select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn),vn,pn;
\d rollup_view;
create view gs_view as select cn,vn,grouping(vn,cn) from sale group by grouping sets ((vn), (cn), (), (cn,vn));
\d gs_view;

-- GROUP_ID function --

select pn, sum(qty), group_id() from sale group by rollup(pn);
select pn, sum(qty), group_id() from sale group by rollup(pn),pn;

--begin equivalent
select pn, sum(qty), 0 from sale group by cn,pn
union all select pn, sum(qty), 1 from sale group by cn,pn
union all select pn, sum(qty), 2 from sale group by cn,pn
union all select pn, sum(qty), 3 from sale group by cn,pn
union all select pn, sum(qty), 4 from sale group by cn,pn
union all select pn, sum(qty), 5 from sale group by cn,pn
union all select pn, sum(qty), 6 from sale group by cn,pn
union all select null, sum(qty), 0 from sale group by cn
union all select null, sum(qty), 1 from sale group by cn
union all select null, sum(qty), 2 from sale group by cn
union all select pn, sum(qty), 0 from sale group by pn
union all select null, sum(qty), 0 from sale;
select pn, sum(qty), group_id() from sale group by rollup(cn,pn), cube(cn,pn);
--end equivalent

-- Having clause --

--begin equivalent
select cn,sum(qty) from sale group by cn,vn having vn=10;
select cn,sum(qty) from sale group by rollup(cn,vn) having vn=10;
--end equivalent

--begin equivalent
select cn, vn, pn, count(dt) from sale group by cn, vn, pn having count(dt) <=2
union all
select cn, vn, null, count(dt) from sale group by cn, vn having count(dt) <=2
union all
select cn, null, null, count(dt) from sale group by cn having count(dt) <=2
union all
select null, null, null, count(dt) from sale having count(dt) <=2;    
select cn, vn, pn, count(dt) from sale group by rollup (cn,vn,pn) having count(dt) <=2;
--end equivalent

--begin equivalent
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn having count(distinct dt) <=2
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn having count(distinct dt) <=2
union all
select cn, null, null, count(distinct dt) from sale group by cn having count(distinct dt) <=2
union all
select null, null, null, count(distinct dt) from sale having count(distinct dt) <=2;    
select cn, vn, pn, count(distinct dt) from sale group by rollup (cn,vn,pn) having count(distinct dt) <=2;
--end equivalent

--begin equivalent
select cn,dt,count(vn),0 as grouping from sale group by cn,dt having count(vn) > 2
union all select cn,null,count(vn),1 as grouping from sale group by cn having count(vn) > 2
union all (select null,null,count(vn),3 as grouping from sale  having count(vn) > 2 limit 1)
union all select null,dt,count(vn),2 as grouping from sale group by dt having count(vn) > 2;
select cn,dt,count(vn),grouping(cn,dt) from sale group by cube(cn,dt) having count(vn) > 2;
select cn,dt,count(vn),grouping(cn,dt) from sale group by cube(dt,cn) having count(vn) > 2;
--end equivalent

--begin equivalent
select cn,dt,count(distinct vn),0 as grouping from sale group by cn,dt having count(distinct vn) > 2
union all select cn,null,count(distinct vn),1 as grouping from sale group by cn having count(distinct vn) > 2
union all (select null,null,count(distinct vn),3 as grouping from sale  having count(distinct vn) > 2 limit 1)
union all select null,dt,count(distinct vn),2 as grouping from sale group by dt having count(distinct vn) > 2;
select cn,dt,count(distinct vn),grouping(cn,dt) from sale group by cube(cn,dt) having count(distinct vn) > 2;
select cn,dt,count(distinct vn),grouping(cn,dt) from sale group by cube(dt,cn) having count(distinct vn) > 2;
--end equivalent

--begin equivalent
select cn, null as vn, null as pn, count(dt), 1 as grouping from sale group by cn;
select cn, vn, pn, count(dt),grouping(cn,vn) from sale group by rollup (cn,vn,pn) having grouping(cn,vn)=1;
--end equivalent

--begin equivalent
select cn, vn, pn, count(dt) from sale group by cn,vn,pn
union all select cn, null, null, count(dt) from sale group by cn;
select cn, vn, pn, count(dt) from sale group by grouping sets ((cn,vn,pn), (cn));
select cn, vn, pn, count(dt) from sale group by grouping sets ((cn,vn,pn), (cn), (cn), (cn))
having group_id()=0;
--end equivalent

--begin equivalent
select cn,vn,pn,count(dt),0,0 as grouping from sale group by cn,vn,pn
union all select cn,null,null,count(dt),0,3 from sale group by cn
union all select cn,null,null,count(dt),1,3 from sale group by cn
order by 6;
-- order 6
select cn, vn, pn, count(dt),group_id(),grouping(cn,vn,pn) from sale group by grouping sets ((cn,vn,pn), (cn), (cn)) order by grouping(cn,vn,pn);
--end equivalent
