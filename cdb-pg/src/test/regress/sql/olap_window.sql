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

select * from customer
window
    w1 as (),
    w2 as (w1 partition by cn); --error - can't partition if referencing ordering window

select * from customer
window
    w1 as (partition by a order by b),
    w2 as (w1 order by cn); --error - can't reorder the ordering window

select * from customer
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



---- 4 -- Partitioned, non-ordered window specifications -- OVER (PARTTION BY ...) ----

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

select cn, vn, pn, 
    row_number() over (partition by cn), 
    row_number() over (partition by vn), 
    row_number() over (partition by pn)
from sale; -- mvd 1->4; 2->5; 3->6

select cn, vn, pn, avg(qty) over (partition by cn) 
from sale;

select cn, vn, pn, avg(qty) over (partition by vn) 
from sale;

select avg(qty) over (partition by cn) 
from sale;

select ntile(3) over (order by cn) 
from sale;

select dt, ntile(5) over (order by dt) 
from sale;

select cn, dt, ntile(3) over (partition by cn order by dt) 
from sale;

select cn, dt, 
    ntile(3) over (partition by cn order by dt), 
    sum(prc) over (order by cn, dt) 
from sale;

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
order by 1,5;

select
  grouping(cn, vn, pn),
  cn, vn, pn,
  sum(qty*prc),
  rank() over (partition by grouping(cn,vn,pn) order by sum(qty*prc))
from sale
group by rollup(cn,vn,pn)
order by 1,5;

-- end equivalent
