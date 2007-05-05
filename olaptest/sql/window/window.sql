select row_number() over (), * from sale;
select row_number() over (w), * from sale window w as () order by 1 desc;
select row_number() over (w), * from sale order by 1 desc window w as (); -- fail! window can't follow order by
select * from customer window w1 as (), w2 as (w1 partition by pn); -- fail! can't partition if referencing ordering window
select * from customer window w1 as (partition by a order by b), w2 as (w1 order by c); -- fail! can't reorder the ordering window
select * from customer window w1 as (order by a rows unbounded preceding), w2 as (w1); -- fail! ordering window can't specify framing
select * from customer window w as (partition by p), wa as (w order by a), wb as (w order by b), waf as (wa rows between 2 preceding and 3 preceding), wbf as (wb range between 2 preceding and 3 preceding); 
select * from customer window w as (partition by x), w as (w order by s); -- fail! can't refer to window in its definition
select * from customer window w as (w order by s); -- fail! can't refer to window in its definition 
select row_number() over (partition by cn), cn, pn from sale;
select row_number() over (partition by pn), cn, pn from sale;
select rank() over (), * from sale; -- fail! order by req'd
select row_number() over (partition by pn, cn), cn, pn from sale;
select rank() over (partition by cn), cn, pn from sale; -- fail! rank requires partitioning
select dense_rank() over (partition by cn), cn, pn from sale; -- fail! dense_rank requires partitioning
select row_number() over (partition by pname, cname) from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
select row_number() over (partition by vn), vn, count(*) from sale group by vn;
select row_number() over (partition by count(*)), vn, count(*) from sale group by vn;
select row_number() over (order by cn), cn, pn from sale;
select row_number() over (order by pn desc), cn, pn from sale;
select row_number() over (order by pn, cn desc), cn, pn from sale;
select rank() over (order by cn), cn, pn from sale;
select dense_rank() over (), * from sale; -- fail! order by req'd
select rank() over (order by pn desc), cn, pn from sale;
select rank() over (order by pn, cn desc), cn, pn from sale;
select dense_rank() over (order by cn), cn, pn from sale;
select dense_rank() over (order by pn desc), cn, pn from sale;
select dense_rank() over (order by pn, cn desc), cn, pn from sale;
select rank() over (w), cn, pn from sale window w as (order by cn);
select rank() over (w), cn, pn from sale window w as (order by pn desc);
select rank() over (w), cn, pn from sale window w as (order by pn, cn desc);
select row_number() over (order by pname, cname) from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
select row_number() over (order by vn), vn, count(*) from sale group by vn;
select row_number() over (), pn, cn, vn, dt, qty, prc from sale;
select row_number() over (order by count(*)), vn, count(*) from sale group by vn;
select row_number() over (),* from customer;
select cn*100 + row_number() over (), cname, row_number() over () -1 from customer;
select row_number() over (), a,b from (select cn, count(*) from sale group by cn) r(a,b);
select * from (select row_number() over () from customer) r(a), (select row_number() over () from sale) s(a) where s.a = r.a;
select row_number() over (), * from sale;
select row_number() over (order by cn), * from sale;
select row_number() over (order by pn,vn), * from sale;
select rank() over (), * from sale; -- fail! order by req'd
select rank() over (order by cn), * from sale;
select row_number() over (), s.pn, s.cn, s.vn, s.dt, s.qty, s.prc, c.cname, c.cloc, p.pname, p.pcolor from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
select rank() over (order by pn,vn), * from sale;
select dense_rank() over (), * from sale; -- fail! order by req'd
select dense_rank() over (order by cn), * from sale;
select dense_rank() over (order by pn,vn), * from sale;
select row_number() over (w), rank() over (w), dense_rank() over (w), * from sale window w as (order by cn);
select row_number() over (order by cn), count(*) over (order by cn) from sale; -- same
select cn, row_number() over (w), count(*) over (w), rank() over (w), dense_rank() over (w), vn from sale window w as (partition by cn order by vn);
select cn,vn, rank() over (order by cn), rank() over (order by cn,vn) from sale;
select cn,vn,pn, row_number() over (partition by cn), rank() over (partition by cn order by vn), rank() over (partition by cn order by vn,pn) from sale;
select cn,vn, row_number() over (partition by cn,vn), cn,vn, rank() over (partition by cn,vn order by vn), cn,vn,pn, rank() over (partition by cn,vn order by vn,pn) from sale;
select row_number() over (), vn, count(*), sum(qty*prc) as amt from sale group by vn;
select dense_rank() over (order by pname, cname), cname, pname from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
select pn, cn, prc*qty, row_number() over (partition by pn), avg(prc*qty) over (partition by pn), avg(prc*qty) over (partition by pn order by cn), percent_rank() over (partition by pn order by cn), rank() over (partition by pn order by cn) from sale;
select pn, row_number() over (partition by pn), avg(pn) over (partition by pn) from sale; 
select pn, row_number() over (partition by pn), avg(pn) over (partition by pn) from sale order by pn;
select cn, row_number() over (partition by cn), avg(cn) over (partition by cn) from sale;
select cn, row_number() over (partition by cn) from sale;
select cn, vn, pn, row_number() over (partition by cn), row_number() over (partition by vn), row_number() over (partition by pn) from sale;
select cn, vn, pn, avg(qty) over (partition by cn) from sale;
select cn, vn, pn, avg(qty) over (partition by vn) from sale;
select cn, vn, pn, avg(qty) over (partition by pn) from sale;
select row_number() over (), name, loc from (select vname, vloc from vendor union select cname, cloc from customer) r(name,loc);
select avg(qty) over (partition by cn) from sale;
select avg(qty) over (partition by vn) from sale;
select avg(qty) over (partition by pn) from sale;
select ntile(3) over (order by cn) from sale;
select dt, ntile(5) over (order by dt) from sale;
select cn, dt, ntile(3) over (partition by cn order by dt) from sale;
select cn, dt, ntile(3) over (partition by cn order by dt), sum(prc) over (order by cn, dt) from sale;
select cn, dt, percent_rank() over (partition by cn order by dt), sum(prc) over (order by cn, dt) from sale;
select row_number() over (), s from generate_series(1,3) r(s);
select row_number() over (), u, v from ( values (1,2),(3,4),(5,6) ) r(u,v);
