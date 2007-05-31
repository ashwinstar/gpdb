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

--begin equivalent
select cn, vn, pn, count(distinct dt) from sale group by cn, vn, pn
union all
select cn, vn, null, count(distinct dt) from sale group by cn, vn
union all
select cn, null, null, count(distinct dt) from sale group by cn
union all
select null, null, null, count(distinct dt) from sale;    
select cn, vn, pn, count(distinct dt) from sale group by rollup(cn,vn,pn);
select cn, vn, pn, count(distinct dt) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn));
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
order by 1,2,3;
select cn, vn, pn, count(distinct dt) from sale group by cube (cn, vn, pn) order by 1,2,3;
select cn, vn, pn, count(distinct dt) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3;
--end equivalent

--begin equivalent order 1,2,3
select cn, vn, pn, sum(qty*prc) from sale group by cn, vn, pn
union all
select cn, vn, null, sum(qty*prc) from sale group by cn, vn
union all
select cn, null, null, sum(qty*prc) from sale group by cn
union all
select null, null, null, sum(qty*prc) from sale 
order by 1,2,3;    
select cn, vn, pn, sum(qty*prc) from sale group by rollup(cn,vn,pn) order by 1,2,3;
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by 1,2,3;
--end equivalent

--begin equivalent order 3,4,1
select pn, sum(qty*prc), cn, vn from sale group by cn, vn, pn
union all
select null, sum(qty*prc), cn, vn from sale group by cn, vn
union all
select null, sum(qty*prc), cn, null from sale group by cn
union all
select null, sum(qty*prc), null, null from sale 
order by cn, vn, pn;    
select pn, sum(qty*prc), cn, vn from sale group by rollup(cn,vn,pn) order by cn, vn, pn;
select pn, sum(qty*prc), cn, vn from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by cn, vn, pn;
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
order by 1,2,3;
select cn, vn, pn, sum(qty*prc) from sale group by cube (cn, vn, pn) order by 1,2,3;
select cn, vn, pn, sum(qty*prc) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3;
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
order by 1,2,3;    
select cn, vn, pn, count(distinct dt) from sale group by rollup(cn,vn,pn) order by 1,2,3;
select cn, vn, pn, count(distinct dt) from sale group by grouping sets((), (cn), (cn,vn), (cn,vn,pn)) order by 1,2,3;
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
order by 1,2,3;
select cn, vn, pn, count(distinct dt) from sale group by cube (cn, vn, pn) order by 1,2,3;
select cn, vn, pn, count(distinct dt) from sale group by grouping sets ((), (cn), (vn), (pn), (cn,vn), (cn,pn), (vn,pn), (cn,vn,pn)) order by 1,2,3;
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


-- GROUPING function -- 

select grouping(cn,vn,pn) from sale;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by cn,vn,pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn),vn,pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn,vn), pn;
select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn);
select cn,vn,pn,grouping(cn,vn,pn) from sale group by cn, rollup(vn,pn);
select cn,vn,pn,grouping(cn,vn,pn) from sale group by vn, rollup(cn, pn);
select grouping(cn), grouping(vn), grouping(pn), cn, vn, pn, count(*) from sale group by rollup(cn,vn,pn);
select grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn) order by 1 desc;
select grouping(cn), grouping(vn), grouping(pn) from sale group by rollup(cn,vn,pn) order by 1 desc, 2 desc, 3 desc;
select cn+vn,pn, grouping(cn+vn,pn), count(*) from sale group by rollup(cn+vn,pn);

-- Ungrouped attributes in GROUPING function --

select grouping(cn,vn,pn) from sale group by cn; --error
select grouping(cn,vn,pn) from sale group by rollup(cn); --error

-- GROUP_ID function --

select pn, sum(qty), group_id() from sale group by rollup(pn);
select pn, sum(qty), group_id() from sale group by rollup(pn),pn;
