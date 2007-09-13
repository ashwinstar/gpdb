--
-- OLAP_WINDOW 
--


---- 1 -- Null window specification -- OVER () ----

select row_number() over (), cn,pn,vn 
from sale; -- mvd 1->1

select rank() over (), * 
from sale; --error - order by req'd

select dense_rank() over (), * 
from sale; --error - order by order by req'd


-- 2 -- Plan trivial windows over various input plan types ----

select row_number() over (), pn, cn, vn, dt, qty, prc
from sale; -- mvd 1->1

select row_number() over (), s.pn, s.cn, s.vn, s.dt, s.qty, s.prc, c.cname, c.cloc, p.pname, p.pcolor
from sale s, customer c, product p 
where s.cn = c.cn and s.pn = p.pn; -- mvd 1->1

select row_number() over (), vn, count(*), sum(qty*prc) as amt
from sale group by vn; -- mvd 1->1

select row_number() over (), name, loc
from (select vname, vloc from vendor union select cname, cloc from customer) r(name,loc); -- mvd 1->1

select row_number() over (), s 
from generate_series(1,3) r(s); -- mvd 1->1

select row_number() over (), u, v
from ( values (1,2),(3,4),(5,6) ) r(u,v); -- mvd 1->1


---- 3 -- Exercise WINDOW Clause ----

select row_number() over (w), * from sale window w as () -- mvd 1->1
order by 1 desc; -- order 1

select row_number() over (w), * from sale
order by 1 desc
window w as (); --error - window can't follow order by

select  count(*) over (w2) from customer
window
    w1 as (),
    w2 as (w1 partition by cn); --error - can't partition if referencing ordering window

select count(*) over (w2) from customer
window
    w1 as (partition by a order by b),
    w2 as (w1 order by cn); --error - can't reorder the ordering window

select  count(*) over (w2) from customer
window 
    w1 as (order by cn rows unbounded preceding),
    w2 as (w1); --error - ordering window can't specify framing

select * from sale
window 
    w as (partition by pn),
    wa as (w order by cn),
    wb as (w order by vn),
    waf as (wa rows between 2 preceding and 3 preceding),
    wbf as (wb range between 2 preceding and 3 preceding);
    
select * from customer
window 
    w as (partition by cn), 
    w as (w order by cname); --error - can't refer to window in its definition

select * from customer
window w as (w order by cname); --error - can't refer to window in its definition

-- test the frame parser
select cn, 
  count(*) over (order by cn range '1'::interval preceding) 
  from customer; -- error, type mismatch

select cn,
  count(*) over (order by cn range between '-11' preceding and '-2' following)
  from customer; -- error, negative values not permitted

select cn,
  count(*) over (order by cn range (2 - 1) preceding)
  from customer; -- error, we don't permit expressions

select cn,
  sum(cn) over (order by cn range NULL preceding)
  from customer; -- we don't permit NULL

select cn,
  sum(cn) over (order by cn range '1'::float8 preceding)
  from customer; -- this, however, should work

---- 4 -- Partitioned, non-ordered window specifications -- OVER (PARTTION BY ...) ----

-- !
select row_number() over (partition by cn), cn, pn 
from sale; -- mvd 2->1

select row_number() over (partition by pn, cn), cn, pn 
from sale; -- mvd 2,3->1

select rank() over (partition by cn), cn, pn 
from sale; --error - rank requires ordering

select dense_rank() over (partition by cn), cn, pn 
from sale; --error - dense_rank requires ordering

select row_number() over (partition by pname, cname), pname, cname
from sale s, customer c, product p 
where s.cn = c.cn and s.pn = p.pn; -- mvd 2,3->1

select row_number() over (partition by vn), vn, count(*)
from sale 
group by vn; -- mvd 2->1

select row_number() over (partition by count(*)), vn, count(*)
from sale 
group by vn; -- mvd 3->1



---- 5 -- Ordered, non-partitioned window specifications -- OVER (ORDER BY ...) ----

select row_number() over (order by cn), cn, pn 
from sale; -- mvd 2->1

select row_number() over (order by pn desc), cn, pn 
from sale; -- mvd 3->1

select row_number() over (order by pn, cn desc), cn, pn 
from sale; -- mvd 2,3->1

select rank() over (order by cn), cn, pn 
from sale; -- mvd 1->-- mvd

select rank() over (order by pn desc), cn, pn 
from sale; -- mvd 1->2,3

select rank() over (order by pn, cn desc), cn, pn 
from sale; -- mvd 1->2,3

select dense_rank() over (order by cn), cn, pn 
from sale; -- mvd 1->2,3

select dense_rank() over (order by pn desc), cn, pn 
from sale; -- mvd 1->2,3

select dense_rank() over (order by pn, cn desc), cn, pn 
from sale; -- mvd 1->2,3

select rank() over (w), cn, pn 
from sale 
window w as (order by cn) -- mvd 1->2,3;

select rank() over (w), cn, pn 
from sale 
window w as (order by pn desc); -- mvd 1->2,3

select rank() over (w), cn, pn 
from sale 
window w as (order by pn, cn desc); -- mvd 1->2,3

select row_number() over (order by pname, cname)
from sale s, customer c, product p 
where s.cn = c.cn and s.pn = p.pn; -- mvd 1->1

select row_number() over (order by vn), vn, count(*)
from sale 
group by vn; -- mvd 2->1

select row_number() over (order by count(*)), vn, count(*)
from sale 
group by vn; -- mvd 3->1


---- X -- Miscellaneous (e.g. old bugs, etc.) ----

select 
    cn*100 + row_number() over (), 
    cname, 
    row_number() over () - 1, 
    cn 
from customer; -- mvd 4->1,3

-- !
select row_number() over (), a,b 
from (
    select cn, count(*) 
    from sale 
    group by cn ) r(a,b); -- mvd 1->1

select * 
from (
    select row_number() over () 
    from customer ) r(a), 
    (
    select row_number() over () 
    from sale ) s(a) 
where s.a = r.a;

select 
    row_number() over (w), 
    rank() over (w), 
    dense_rank() over (w), 
    * 
from sale 
window w as (order by cn); -- mvd 4->1; 4->2; 4->3

select cn, row_number() over (w), count(*) over (w), rank() over (w), dense_rank() over (w), vn 
from sale 
window w as (partition by cn order by vn); -- mvd 1,4->2

select cn,vn, rank() over (order by cn), rank() over (order by cn,vn) 
from sale;

-- !
select cn,vn,pn, 
  row_number() over (partition by cn), 
  rank() over (partition by cn order by vn), 
  rank() over (partition by cn order by vn,pn) 
from sale; -- mvd 1->4

select cn,vn, row_number() over (partition by cn,vn),
  cn,vn, rank() over (partition by cn,vn order by vn),
  cn,vn,pn, rank() over (partition by cn,vn order by vn,pn)
from sale; -- mvd 1,2->3

select 
  dense_rank() over (order by pname, cname), cname, pname
from sale s, customer c, product p 
where s.cn = c.cn and s.pn = p.pn;

select pn, cn, prc*qty, 
    row_number() over (partition by pn),
    avg(prc*qty) over (partition by pn),
    avg(prc*qty) over (partition by pn order by cn),
    percent_rank() over (partition by pn order by cn),
    rank() over (partition by pn order by cn)
from sale; -- mvd 1->4

select pn, row_number() over (partition by pn), avg(pn) over (partition by pn) 
from sale; -- mvd 1->2

select pn, row_number() over (partition by pn), avg(pn) over (partition by pn) 
from sale order by pn; -- mvd 1->2

select cn, row_number() over (partition by cn), avg(cn) over (partition by cn) 
from sale; -- mvd 1->2

select cn, row_number() over (partition by cn) 
from sale; -- mvd 1->2

-- !
select cn, vn, pn, 
    row_number() over (partition by cn), 
    row_number() over (partition by vn), 
    row_number() over (partition by pn)
from sale; -- mvd 1->4; 2->5; 3->6

select cn, vn, pn, avg(qty) over (partition by cn) 
from sale;

select cn, vn, pn, avg(qty) over (partition by vn) 
from sale;

-- !
select avg(qty) over (partition by cn) 
from sale;

select ntile(3) over (order by cn) 
from sale;

select dt, ntile(5) over (order by dt) 
from sale;

-- !
select cn, dt, ntile(3) over (partition by cn order by dt) 
from sale; -- mvd 1,2->3

-- !
select cn, dt, 
    ntile(3) over (partition by cn order by dt), 
    sum(prc) over (order by cn, dt) 
from sale; -- mvd 1,2->3

-- !
select cn, dt, 
    percent_rank() over (partition by cn order by dt), 
    sum(prc) over (order by cn, dt) 
from sale;

select                                                         
  grouping(cn, vn, pn) as gr, cn, vn, pn,
  sum(qty*prc), rank() over (partition by cn order by sum(qty*prc))
from sale
group by rollup(cn,vn,pn) 
order by 2 desc, 5; -- order 2,5

select                                                         
  grouping(cn, vn, pn) as gr, cn, vn, pn,
  sum(qty*prc), rank() over (partition by cn order by sum(qty*prc))
from sale
group by rollup(cn,vn,pn) 
order by 2, 5; -- order 2,5

select cn, vn, pn, rank() over (partition by vn order by cn) 
from sale;

select row_number() over (partition by vn), 
    cn, vn, pn, 
    rank() over (partition by vn order by cn) 
from sale; -- mvd 3->1

select row_number() over (partition by pn), 
    cn, vn, pn, 
    rank() over (partition by vn order by cn) 
from sale; -- mvd 4->1

select cname, 
	   rank() over (partition by sale.cn order by pn),
	   rank() over (partition by sale.cn order by vn)
from sale, customer
where sale.cn = customer.cn; -- mvd 1->2,3

-- Check that we invert count() correctly. count() is a special case
-- because when not invoked as count(*), it uses "any" as an argument.
-- This can trip us up.
SELECT sale.pn, COUNT(sale.pn) OVER(order by sale.pn)              
FROM sale;

-- Reject DISTINCT qualified aggs when an ORDER BY is present
SELECT count(distinct sale.pn) OVER (order by sale.pn) from sale;

-- Aggregate nesting --

create table olap_tmp(g integer, h integer, i integer, x integer);

insert into olap_tmp values 
    (9,1,1,1),
    (9,1,1,0),
    (9,1,1,1),
    (9,1,1,0),
    (9,1,2,1),
    (9,1,2,0),
    (9,1,2,1),
    (9,1,2,0),
    (9,4,1,1),
    (9,4,1,0),
    (9,4,1,1),
    (9,4,1,0),
    (9,4,2,1),
    (9,4,2,0),
    (9,4,2,1),
    (9,4,2,0),
    (9,1,1,1),
    (9,1,1,0),
    (9,1,1,1),
    (9,1,1,0),
    (9,1,2,1),
    (9,1,2,0),
    (9,1,2,1),
    (9,1,2,0),
    (9,4,1,1),
    (9,4,1,0),
    (9,4,1,1),
    (9,4,1,0),
    (9,4,2,1),
    (9,4,2,0),
    (9,4,2,1),
    (9,4,2,0);


select g,h,i,avg(x) as ax
from olap_tmp
group by g,h,i;

-- begin equivalent

select 
  g, 
  ax, 
  avg(g) over (partition by h order by i), 
  sum(ax) over (partition by i order by g)
from (select g,h,i,avg(x) as ax from olap_tmp group by g,h,i) rr;

select
  g,
  avg(x),
  avg(g) over (partition by h order by i),
  sum(avg(x)) over (partition by i order by g)
from olap_tmp
group by g,h,i;

-- end equivalent

drop table olap_tmp;

-- begin equivalent

select g, cn, vn, pn, s, rank() over (partition by g order by s)
from
  (select
    grouping(cn, vn, pn),
    cn, vn, pn,
    sum(qty*prc)
  from sale
  group by rollup(cn,vn,pn)) olap_tmp(g,cn,vn,pn,s)
order by 1,5; -- order 1,5

select
  grouping(cn, vn, pn),
  cn, vn, pn,
  sum(qty*prc),
  rank() over (partition by grouping(cn,vn,pn) order by sum(qty*prc))
from sale
group by rollup(cn,vn,pn)
order by 1,5; -- order 1,5

-- end equivalent

-- basic framed query test
select pn, count(*) 
over (order by pn range between 1 preceding and 1 following) as c
from sale
order by pn;

-- Some data types, like date, when mixed with operators like "-" result in
-- data returned in a different data type. We're smart enough to detect
-- this but some tests are good.

select cn, dt, qty,
	sum(qty) over (order by dt 
       range between '1 year'::interval preceding and 
                     '1 month'::interval following)
from sale;

select cn, dt, qty, prc,
	sum(qty) over (order by prc range '314.15926535'::float8 preceding) as sum
from sale;

-- There are some types we cannot cast back, test for those too
select cn, dt, qty,
	sum(qty) over (order by dt
       range '2007-08-05'::date preceding) as sum
from sale;

select cn, dt, qty,
	sum(qty) over (order by dt
       range '192.168.1.1'::inet preceding) as sum
from sale;

-- Test for FOLLOWING frames
select cn, prc, dt, sum(prc) over (order by dt,cn rows between 2 following and 3 following) as f from sale;

-- Check that mixing cume_dist() with other window functions on the same
-- window does not result in badness

select cn, rank() over (w), cume_dist() over (w) from
  customer
  window w as (order by cname);

-- Test for MPP-1762

-- begin equivalent

SELECT sale.prc,sale.cn, sale.cn, 
AVG(sale.pn) OVER(order by sale.pn desc,sale.vn asc,sale.cn desc rows between unbounded preceding and unbounded following) as avg,
sale.vn,sale.pn,
DENSE_RANK() OVER(order by sale.pn asc) FROM sale,vendor WHERE sale.vn =vendor.vn;

SELECT sale.prc,sale.cn,sale.cn, AVG(sale.pn) OVER(win1),
sale.vn,sale.pn,
DENSE_RANK() OVER(win2)
FROM sale,vendor                                                       
WHERE sale.vn=vendor.vn
WINDOW win1 as (order by sale.pn desc,sale.vn asc,sale.cn desc rows between unbounded preceding and unbounded following ),
win2 as (order by sale.pn asc);

-- end equivalent

-- Test for MPP-1756, the planner should create just the one key level

explain select cn,
sum(qty) over (order by cn rows between 1 preceding and 1 following),
sum(qty) over (order by cn rows between 1 preceding and 1 following)
from sale;

select cn,
sum(qty) over (order by cn rows between 1 preceding and 1 following),
sum(qty) over (order by cn rows between 1 preceding and 1 following)
from sale;

-- test for MPP-1760. Framed queries without order by clauses are not
-- permitted.

select cn,
sum(count(*)) over (range between 1 preceding and 1 following)
from sale
group by cn;

-- test for issue which reopened MPP-1762
-- We allow the user to specify DESC sort order
SELECT sale.cn,sale.vn,AVG(cast (sale.vn as int)) OVER(order by sale.cn desc) as avg from sale;

-- a bit harder
SELECT sale.cn,sale.dt, sale.vn,AVG(cast (sale.vn as int)) OVER(order by sale.cn desc, sale.dt asc) as avg from sale;

-- Even harder (MPP-1805)
SELECT sale.cn,sale.prc,sale.qty,                                  
SUM(floor(sale.prc*sale.qty)) 
OVER(order by sale.cn desc range between 4  preceding and 4 following) as foo 
FROM sale;

SELECT sale.pn,sale.vn, 
SUM(cast (sale.vn as int)) 
OVER(order by sale.cn desc range current row) as sum,
sale.cn from sale;

-- test for MPP-1810 and other similar queries
SELECT sale.vn,sale.vn,sale.qty,                                   
FIRST_VALUE(floor(sale.vn)) OVER(order by sale.vn asc range 0 preceding) as f
from sale;

select cn, cast(cname as varchar(10)), first_value(cast(cname as varchar(10))) 
  over(order by cn range 2 preceding) as f
from customer;

select cn, prc, dt, first_value(prc) over (order by dt rows between 1 following
and 4 following) as f from sale;

-- test for second issue in MPP-1810
select vn, first_value(vn) over(order by vn range 2 preceding) from vendor;

-- MPP-1819
SELECT sale.cn,sale.pn, 
FIRST_VALUE(sale.pn+sale.pn) OVER(order by sale.cn rows unbounded preceding) as fv from sale;

SELECT sale.cn,sale.pn, 
FIRST_VALUE(sale.pn+sale.pn) OVER(order by sale.cn range unbounded preceding) as fv from sale;

-- MPP-1812
SELECT sale.cn,sale.prc,sale.vn,
SUM(floor(sale.prc*sale.vn)) OVER(order by sale.cn asc,sale.prc range current row) as fv 
from sale;

-- MPP-1843, check the interaction between ROWS frames and partitioning
SELECT sale.dt,sale.prc,sale.cn,
sale.vn,
SUM(sale.cn) OVER(partition by sale.dt,sale.prc order by sale.cn asc rows 
between 0 following and 1 following) as sum 
from sale order by dt, prc, cn;

-- MPP-1804, Used to return incorrect number of rows
SELECT sale.vn,sale.cn,
SUM(sale.cn) OVER(partition by sale.vn order by sale.cn desc range between current row and unbounded following) as sum from sale;

-- MPP-1840, grouping + windowing
--begin_equivalent
SELECT cn, vn, pn, GROUPING(cn,vn,pn),
SUM(vn) OVER (PARTITION BY GROUPING(cn,vn,pn) ORDER BY cn) as sum FROM sale
GROUP BY ROLLUP(cn,vn,pn) order by 4; -- order 4

select cn,vn,pn,grouping,
sum(vn) over (partition by grouping order by cn) as sum 
from (select cn,vn,pn,grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn)) t
order by 4; -- order 4
--end_equivalent

-- MPP-1860
SELECT sale.pn,sale.vn,sale.qty,
SUM(floor(sale.vn*sale.qty)) OVER(order by sale.pn asc rows between 4 preceding and 0 preceding)  as sum 
FROM sale;

-- MPP-1866
SELECT sale.cn,sale.pn,sale.prc, 
SUM(floor(sale.pn*sale.prc)) OVER(order by sale.cn asc, pn rows between current row and unbounded following ) as sum 
FROM sale;

SELECT sale.pn,sale.vn, 
COUNT(floor(sale.vn)) OVER(order by sale.pn asc rows between current row and unbounded following) 
FROM sale;    

-- Sanity test for executor detection of non-sense frames
SELECT sale.pn,sale.cn,sale.prc,
    AVG(cast (sale.prc as int))
      OVER(order by sale.cn asc range between 6 following and 0 following ) as c
      FROM sale;

SELECT sale.pn,sale.cn,sale.prc,
    AVG(cast (sale.prc as int))
      OVER(order by sale.cn asc rows between 6 following and 0 following ) as c
      FROM sale;

SELECT sale.pn,sale.cn,sale.prc,
    AVG(cast (sale.prc as int))
      OVER(order by sale.cn asc range between 1 preceding and 10 preceding ) as c
      FROM sale;

-- Check LEAD()
-- sanity tests
select p.proname, p.proargtypes from pg_window w, pg_proc p, pg_proc p2 where w.winfunc = p.oid and w.winfnoid = p2.oid and p2.proname = 'lead' order by 1,2;

select lead(cn) from sale;

select cn, cname, lead(cname, -1) over (order by cn) from customer;

-- actual LEAD tests
select cn, cname, lead(cname, 2, 'undefined') over (order by cn) from customer;

select cn, cname, lead(cname, 2) over (order by cn) from customer;

select cn, cname, lead(cname) over (order by cn) from customer;

select cn, vn, pn, lead(cn, 1, cn + 1) over (order by cn, vn, pn) from 
sale order by 1, 2, 3;

select cn, vn, pn, qty * prc, 
	lead(qty * prc) over (order by cn, vn, pn) from sale
order by 1, 2, 3;

-- Check LAG()
-- sanity tests
select p.proname, p.proargtypes from pg_window w, pg_proc p, pg_proc p2 where w.winfunc = p.oid and w.winfnoid = p2.oid and p2.proname = 'lag' order by 1,2;

-- actual LAG tests
select cn, cname, lag(cname, 2, 'undefined') over (order by cn) from customer;

select cn, cname, lag(cname, 2) over (order by cn) from customer;

select cn, cname, lag(cname) over (order by cn) from customer;

select cn, vn, pn, lag(cn, 1, cn + 1) over (order by cn, vn, pn) from 
sale order by 1, 2, 3;

select cn, vn, pn, qty * prc, 
	lag(qty * prc) over (order by cn, vn, pn) from sale
order by 1, 2, 3;

-- LAST_VALUE tests
SELECT sale.cn,sale.qty,sale.pn, 
LAST_VALUE(sale.qty*sale.pn) OVER(partition by sale.cn order by sale.cn,pn range between unbounded preceding and unbounded following ) as lv 
from sale order by 1, 2, 3;

SELECT sale.vn,sale.qty,                                   
LAST_VALUE(floor(sale.vn)) OVER(order by sale.vn asc range 0 preceding) as f
from sale;

select cn, cast(cname as varchar(10)), last_value(cast(cname as varchar(10))) 
  over(order by cn range 2 preceding) as f
from customer;

select cn, prc, dt, 
last_value(prc) over (order by dt, prc, cn rows between 1 following and 4 following) as f from sale;

select vn, last_value(vn) 
over(order by vn range between 2 preceding and 2 following) from vendor;

select vn, last_value(vn) 
over(order by vn range between 20 preceding and 20 following) from vendor;

SELECT sale.cn,sale.pn, 
last_VALUE(sale.pn+sale.pn) 
OVER(order by sale.cn rows between current row and 
unbounded following) as fv from sale;

SELECT sale.cn,sale.pn, 
last_VALUE(sale.pn+sale.pn) OVER(order by sale.cn range between current row and
unbounded following) as fv from sale;

-- MPP-1881
SELECT sale.pn,
COUNT(pn) OVER(order by sale.pn asc range between unbounded preceding and unbounded following) 
FROM sale;

-- MPP-1878
SELECT sale.vn,sale.cn, COUNT(vn) OVER(order by sale.cn asc range between unbounded preceding and 2 following)
FROM sale;

-- MPP-1877
SELECT sale.vn,sale.cn, 
COUNT(vn) OVER(order by sale.cn asc range between unbounded preceding and 2 preceding) 
FROM sale; 
SELECT sale.vn,sale.cn, 
COUNT(vn) OVER(order by sale.cn asc rows between unbounded preceding and 2 preceding) 
FROM sale; 

-- Framing clauses
select cn,count(*) over (order by cn rows between 2 preceding and 1 preceding) as c from sale;
select cn,count(*) over (order by cn rows between 2 preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn rows between 2 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn rows between 0 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn rows between 0 following and 1 following) as c from sale;
select cn,count(*) over (order by cn rows between 1 following and 2 following) as c from sale;
select cn,count(*) over (order by cn rows between unbounded preceding and 2 preceding) as c from sale;
select cn,count(*) over (order by cn rows between unbounded preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn rows between unbounded preceding and 2 following) as c from sale;
select cn,count(*) over (order by cn rows between 2 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn rows between 0 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn rows between 0 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn rows between 1 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn rows between unbounded preceding and unbounded following) as c from sale;

select cn,count(*) over (order by cn desc rows between 2 preceding and 1 preceding) as c from sale;
select cn,count(*) over (order by cn desc rows between 2 preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn desc rows between 2 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn desc rows between 0 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn desc rows between 0 following and 1 following) as c from sale;
select cn,count(*) over (order by cn desc rows between 1 following and 2 following) as c from sale;
select cn,count(*) over (order by cn desc rows between unbounded preceding and 2 preceding) as c from sale;
select cn,count(*) over (order by cn desc rows between unbounded preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn desc rows between unbounded preceding and 2 following) as c from sale;
select cn,count(*) over (order by cn desc rows between 2 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc rows between 0 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc rows between 0 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc rows between 1 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc rows between unbounded preceding and unbounded following) as c from sale;

select cn,count(*) over (order by cn range between 2 preceding and 1 preceding) as c from sale;
select cn,count(*) over (order by cn range between 2 preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn range between 2 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn range between 0 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn range between 0 following and 1 following) as c from sale;
select cn,count(*) over (order by cn range between 1 following and 2 following) as c from sale;
select cn,count(*) over (order by cn range between unbounded preceding and 2 preceding) as c from sale;
select cn,count(*) over (order by cn range between unbounded preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn range between unbounded preceding and 2 following) as c from sale;
select cn,count(*) over (order by cn range between 2 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn range between 0 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn range between 0 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn range between 1 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn range between unbounded preceding and unbounded following) as c from sale;

select cn,count(*) over (order by cn desc range between 2 preceding and 1 preceding) as c from sale;
select cn,count(*) over (order by cn desc range between 2 preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn desc range between 2 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn desc range between 0 preceding and 1 following) as c from sale;
select cn,count(*) over (order by cn desc range between 0 following and 1 following) as c from sale;
select cn,count(*) over (order by cn desc range between 1 following and 2 following) as c from sale;
select cn,count(*) over (order by cn desc range between unbounded preceding and 2 preceding) as c from sale;
select cn,count(*) over (order by cn desc range between unbounded preceding and 0 preceding) as c from sale;
select cn,count(*) over (order by cn desc range between unbounded preceding and 2 following) as c from sale;
select cn,count(*) over (order by cn desc range between 2 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc range between 0 preceding and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc range between 0 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc range between 1 following and unbounded following) as c from sale;
select cn,count(*) over (order by cn desc range between unbounded preceding and unbounded following) as c from sale;

-- MPP-1885
SELECT sale.vn,
COUNT(vn) OVER(order by sale.vn asc range between unbounded preceding and 2 preceding)
FROM sale;

-- aggregates in multiple key levels
select cn,pn,vn, count(*) over (order by cn) as c1, 
count(*) over (order by cn,vn) as c2,
count(*) over (order by cn,vn,pn) as c3 from sale;
select cn,pn,vn, count(*) over (order by cn range between 2 preceding and 2 following) as c1,
count(*) over (order by cn,vn rows between 2 preceding and 2 following) as c2,
count(*) over (order by cn,vn,pn) as c3 from sale;

-- MPP-1897
SELECT sale.cn,sale.qty,
SUM(floor(sale.qty)) OVER(order by sale.cn asc range between 2 following and 2 following) 
FROM sale;
SELECT sale.cn,sale.qty,
SUM(floor(sale.qty)) OVER(order by sale.cn asc rows between 2 following and 2 following) as sum
FROM sale;

-- MPP-1892
SELECT sale.vn,sale.cn,sale.prc,sale.pn,sale.prc,
COUNT(floor(sale.prc)) OVER(partition by sale.vn,sale.cn,sale.prc,sale.vn order by sale.pn asc range between unbounded preceding and 1 preceding) as count 
from sale;

-- MPP-1893
SELECT sale.prc,sale.cn,sale.vn,sale.pn,sale.cn,
AVG(floor(sale.pn-sale.cn)) OVER(partition by sale.prc,sale.cn order by sale.vn desc range between 1 preceding and unbounded following) as avg
FROM sale;
SELECT sale.prc,sale.cn,sale.vn,sale.pn,sale.cn,
AVG(floor(sale.pn-sale.cn)) OVER(partition by sale.prc,sale.cn order by sale.vn desc range between 0 preceding and unbounded following) as avg
FROM sale;

-- MPP-1895
SELECT sale.prc,sale.vn,
COUNT(vn) OVER(partition by sale.prc order by sale.vn asc range between 3 preceding and 3 following) 
FROM sale;

-- MPP-1898
SELECT sale.vn,sale.qty,
SUM(floor(sale.qty)) OVER(order by sale.vn desc range between 0 following and 2 following ) as sum
from sale;

-- MPP-1907, MPP-1912
-- begin_equivalent
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc rows between 4 preceding and current row) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc rows between 4 preceding and 0 preceding) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc rows between 4 preceding and 0 following) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc rows 4 preceding) as count 
FROM sale;
-- end_equivalent

-- begin_equivalent
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc range between 4 preceding and current row) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc range between 4 preceding and 0 preceding) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc range between 4 preceding and 0 following) as count 
FROM sale;
SELECT sale.pn,
COUNT(floor(sale.pn)) OVER(order by sale.pn desc range 4 preceding) as count 
FROM sale;
-- end_equivalent

-- MPP-1915
select cn, qty, sum(qty) over(order by cn) as sum, cume_dist() over(order by cn) as cume1 from sale;
SELECT sale.cn,
SUM(sale.cn) OVER(order by sale.cn asc range 1 preceding ),COUNT(floor(sale.vn)) OVER(order by sale.cn asc range 1 preceding ) 
FROM sale;
SELECT sale.cn,
SUM(sale.cn) OVER(order by sale.cn asc range 2 preceding ),COUNT(floor(sale.vn)) OVER(order by sale.cn asc range 1 preceding ) 
FROM sale;

select pn, count(*) over (order by pn range between 100 preceding and 100 following),
count(*) over (order by pn range between 200 preceding and 200 following) from sale;

-- MPP-1923
SELECT sale.cn,sale.pn,sale.vn,
CUME_DIST() OVER(partition by sale.cn,sale.pn order by sale.vn desc,sale.pn desc,sale.cn asc)
FROM sale;

SELECT sale.cn,sale.vn,sale.pn,
SUM(floor(sale.cn*sale.vn)) OVER(partition by sale.vn,sale.pn order by sale.pn asc rows between 1 following and unbounded following) as sum
from sale;

-- MPP-1924
SELECT sale.cn,
COUNT(cn) OVER(order by sale.cn range between 7 following and 7 following) as count 
FROM sale;

-- MPP-1925
SELECT sale.pn,sale.vn,
COUNT(floor(sale.cn-sale.prc)) OVER(order by sale.pn asc,sale.vn asc,sale.vn desc rows between current row and 2 following ) as count,sale.pn,
CUME_DIST() OVER(order by sale.pn asc) as cume_dist
FROM sale;

-- MPP-1874
CREATE TABLE FACT_TEST
(DATE_KEY    NUMERIC(10,0)  NOT NULL,
 BCOOKIE     text  NOT NULL,
 TIME_KEY    INTEGER  NOT NULL,
 EVENT_TYPE  text  NOT NULL
);
insert into fact_test values (20070101, 'W', 100, 'c');
insert into fact_test values (20070101, 'W', 100, 'p');
insert into fact_test values (20070101, 'B', 100, 'c');
insert into fact_test values (20070101, 'A', 10100101, 'p');
insert into fact_test values (20070101, 'A', 20100101, 'p');
insert into fact_test values (20070101, 'B', 101, 'p');
insert into fact_test values (20070101, 'C', 105, 'p');
insert into fact_test values (20070101, 'D', 107, 'p');
insert into fact_test values (20070101, 'E', 10, 'c');
insert into fact_test values (20070101, 'E', 10, 'p');
insert into fact_test values (20070101, 'A', 101, 'c');
insert into fact_test values (20070101, 'A', 101, 'p');
insert into fact_test values (20070101, 'A', 10100101, 'p');

select date_key, bcookie, time_key, event_type, 
(
 case
   when (time_key- lag(time_key, 1, NULL) over (
         partition by date_key, bcookie
         order by date_key, bcookie, time_key, event_type)
   ) > 1800 then 1
            else 0
 end
) AS time_check
from fact_test;
drop table fact_test;

-- MPP-1929
SELECT vn,pn,cn,
TO_CHAR(COALESCE(CUME_DIST() OVER(partition by sale.vn,sale.pn order by sale.cn desc),0),'99999999.9999999') as cum_dist,
TO_CHAR(COALESCE(CUME_DIST() OVER(partition by sale.vn,sale.pn order by sale.cn desc),0),'99999999.9999999') as cum_dist
from sale;

select cn,pn,lag(pn,cn) over (order by pn) from sale;
select cn,pn,lead(pn,cn) over (order by pn) from sale;
select vn,cn,pn,lead(pn,cn) over (partition by vn order by cn) from sale;
select vn,cn,pn,lag(pn,cn) over (partition by vn order by cn) from sale;

-- MPP-1934
SELECT sale.dt,sale.vn,sale.qty,sale.pn,sale.cn,
LEAD(cast(floor(sale.cn+sale.vn) as int),cast (floor(sale.qty) as int),NULL) OVER(partition by sale.dt,sale.vn,sale.qty order by sale.pn desc,sale.cn desc) as lead
FROM sale;

-- MPP-1935
SELECT sale.vn,sale.qty,sale.prc,
LAG(cast(floor(sale.qty*sale.prc) as int),cast (floor(sale.prc) as int),NULL) OVER(order by sale.vn asc) as lag 
from sale;

-- MPP-1936
SELECT sale.vn,sale.prc,sale.qty,
COUNT(floor(sale.prc)) OVER(order by sale.vn asc),
LAG(cast(floor(sale.qty*sale.prc) as int),cast (floor(sale.prc) as int),NULL) OVER(order by sale.vn asc) as lag 
from sale;

select cn,vn,qty,
sum(qty) over(order by cn rows between 1 preceding and 0 following) as sum1,
sum(qty) over(order by cn rows between 1 preceding and 1 following) as sum2
from sale;

-- MPP-1933
SELECT sale.prc,sale.vn,sale.cn,sale.pn,sale.qty,
LAG(cast(floor(sale.vn) as int),cast (floor(sale.prc) as int),NULL) OVER(partition by sale.prc,sale.vn,sale.cn,sale.pn order by sale.cn desc) as lag1
from sale;
SELECT sale.prc,sale.vn,sale.cn,sale.pn,sale.qty,
LAG(cast(floor(sale.qty/sale.vn) as int),cast (floor(sale.pn) as int),NULL) OVER(partition by sale.prc,sale.vn,sale.cn,sale.pn order by sale.cn desc) as lag2
from sale;
SELECT sale.prc,sale.vn,sale.cn,sale.pn,sale.qty,
LAG(cast(floor(sale.vn) as int),cast (floor(sale.prc) as int),NULL) OVER(partition by sale.prc,sale.vn,sale.cn,sale.pn order by sale.cn desc) as lag1,
LAG(cast(floor(sale.qty/sale.vn) as int),cast (floor(sale.pn) as int),NULL) OVER(partition by sale.prc,sale.vn,sale.cn,sale.pn order by sale.cn desc) as lag2
from sale;

-- MIN/MAX
select cn,vn,min(vn) over (order by cn rows between 2 preceding and 1 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 2 preceding and 0 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 2 preceding and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 0 preceding and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 0 following and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 1 following and 2 following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between unbounded preceding and 2 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn rows between unbounded preceding and 0 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn rows between unbounded preceding and 2 following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 2 preceding and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 0 preceding and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 0 following and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between 1 following and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn rows between unbounded preceding and unbounded following) as min from sale;

select cn,vn,min(vn) over (order by cn desc rows between 2 preceding and 1 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 2 preceding and 0 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 2 preceding and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 0 preceding and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 0 following and 1 following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 1 following and 2 following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between unbounded preceding and 2 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between unbounded preceding and 0 preceding) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between unbounded preceding and 2 following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 2 preceding and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 0 preceding and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 0 following and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between 1 following and unbounded following) as min from sale;
select cn,vn,min(vn) over (order by cn desc rows between unbounded preceding and unbounded following) as min from sale;

select pn,vn,max(vn) over (order by pn range between 200 preceding and 150 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn range between 200 preceding and 0 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn range between 200 preceding and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 0 preceding and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 0 following and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 150 following and 200 following) as max from sale;
select pn,vn,max(vn) over (order by pn range between unbounded preceding and 200 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn range between unbounded preceding and 0 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn range between unbounded preceding and 200 following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 200 preceding and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 0 preceding and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 0 following and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn range between 150 following and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn range between unbounded preceding and unbounded following) as max from sale;

select pn,vn,max(vn) over (order by pn desc range between 200 preceding and 150 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 200 preceding and 0 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 200 preceding and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 0 preceding and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 0 following and 150 following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 150 following and 200 following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between unbounded preceding and 200 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between unbounded preceding and 0 preceding) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between unbounded preceding and 200 following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 200 preceding and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 0 preceding and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 0 following and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between 150 following and unbounded following) as max from sale;
select pn,vn,max(vn) over (order by pn desc range between unbounded preceding and unbounded following) as max from sale;

select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 2 preceding and 1 preceding) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 2 preceding and 0 preceding) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 2 preceding and 1 following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 0 preceding and 1 following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 0 following and 1 following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 1 following and 2 following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between unbounded preceding and 2 preceding) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between unbounded preceding and 0 preceding) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between unbounded preceding and 2 following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 2 preceding and unbounded following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 0 preceding and unbounded following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 0 following and unbounded following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between 1 following and unbounded following) as min from sale;
select cn,vn,pn,min(vn) over (partition by cn order by pn rows between unbounded preceding and unbounded following) as min from sale;

select cn,pn,vn,max(vn) over (partition by cn order by pn range between 200 preceding and 150 preceding) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 200 preceding and 0 preceding) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 200 preceding and 150 following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 0 preceding and 150 following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 0 following and 150 following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 150 following and 200 following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between unbounded preceding and 200 preceding) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between unbounded preceding and 0 preceding) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between unbounded preceding and 200 following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 200 preceding and unbounded following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 0 preceding and unbounded following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 0 following and unbounded following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between 150 following and unbounded following) as max from sale;
select cn,pn,vn,max(vn) over (partition by cn order by pn range between unbounded preceding and unbounded following) as max from sale;

-- MPP-1943
SELECT sale.cn,sale.pn,sale.vn,
MAX(floor(sale.pn/sale.vn)) OVER(order by sale.cn asc,sale.cn asc,sale.pn asc)
FROM sale;

-- MPP-1944
SELECT sale.pn,cn,dt,pn,prc,
MAX(floor(sale.prc+sale.prc)) OVER(partition by sale.pn,sale.cn,sale.dt,sale.pn order by sale.pn desc,sale.pn asc rows between current row and unbounded following ) 
FROM sale;

select qty,cn,pn,dt,pn,prc,
MAX(floor(sale.prc+sale.prc)) OVER(partition by sale.qty,sale.cn,sale.pn,sale.dt order by sale.pn asc range between 0 following and unbounded following ) 
from sale;

-- MPP-1947
SELECT dt,cn,cn,qty,
MIN(floor(sale.qty)) OVER(partition by sale.dt,sale.cn,sale.dt order by sale.cn desc range between 1 preceding and unbounded following ) 
FROM sale;

SELECT dt,cn,cn,qty, 
MAX(floor(sale.qty)) OVER(partition by sale.dt,sale.cn,sale.dt order by sale.cn desc range between 1 preceding and unbounded following )
from sale;

-- FIRST_VALUE/LAST_VALUE
select cn,vn,first_value(vn) over (order by cn rows between 2 preceding and 1 preceding) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 2 preceding and 0 preceding) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 2 preceding and 1 following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 0 preceding and 1 following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 0 following and 1 following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 1 following and 2 following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between unbounded preceding and 2 preceding) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between unbounded preceding and 0 preceding) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between unbounded preceding and 2 following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 2 preceding and unbounded following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 0 preceding and unbounded following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 0 following and unbounded following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between 1 following and unbounded following) as first_value from sale;
select cn,vn,first_value(vn) over (order by cn rows between unbounded preceding and unbounded following) as first_value from sale;

select pn,vn,last_value(vn) over (order by pn desc range between 200 preceding and 150 preceding) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 200 preceding and 0 preceding) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 200 preceding and 150 following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 0 preceding and 150 following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 0 following and 150 following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 150 following and 200 following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between unbounded preceding and 200 preceding) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between unbounded preceding and 0 preceding) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between unbounded preceding and 200 following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 200 preceding and unbounded following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 0 preceding and unbounded following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 0 following and unbounded following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between 150 following and unbounded following) as last_value from sale;
select pn,vn,last_value(vn) over (order by pn desc range between unbounded preceding and unbounded following) as last_value from sale;

select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 2 preceding and 1 preceding) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 2 preceding and 0 preceding) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 2 preceding and 1 following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 0 preceding and 1 following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 0 following and 1 following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 1 following and 2 following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between unbounded preceding and 2 preceding) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between unbounded preceding and 0 preceding) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between unbounded preceding and 2 following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 2 preceding and unbounded following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 0 preceding and unbounded following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 0 following and unbounded following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between 1 following and unbounded following) as last_value from sale;
select cn,vn,pn,last_value(vn) over (partition by cn order by pn rows between unbounded preceding and unbounded following) as last_value from sale;

select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 200 preceding and 150 preceding) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 200 preceding and 0 preceding) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 200 preceding and 150 following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 0 preceding and 150 following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 0 following and 150 following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 150 following and 200 following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between unbounded preceding and 200 preceding) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between unbounded preceding and 0 preceding) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between unbounded preceding and 200 following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 200 preceding and unbounded following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 0 preceding and unbounded following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 0 following and unbounded following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between 150 following and unbounded following) as first_value from sale;
select cn,pn,vn,first_value(vn) over (partition by cn order by pn range between unbounded preceding and unbounded following) as first_value from sale;

-- MPP-1957
select dt,pn,qty,
LEAD(cast(floor(sale.qty/sale.qty) as int),cast (floor(sale.qty) as int),NULL) OVER(partition by sale.dt order by sale.pn asc) as lead 
from sale;
