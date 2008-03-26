-- $ID$
-- TPC-H/TPC-R Potential Part Promotion Query (Q20)
-- Function Query Definition
-- Approved February 1998
:x
/************* Query 20 modified to remove correlation *****
:o
select
	s_name,
	s_address
from
	supplier,
	nation
where
	s_suppkey in (
		select
			ps_suppkey
		from
			partsupp
		where
			ps_partkey in (
				select
					p_partkey
				from
					part
				where
					p_name like ':1%'
			)
			and ps_availqty > (
				select
					0.5 * sum(l_quantity)
				from
					lineitem
				where
					l_partkey = ps_partkey
					and l_suppkey = ps_suppkey
					and l_shipdate >= date ':2'
					and l_shipdate < date ':2' + interval '1 year'
			)
	)
	and s_nationkey = n_nationkey
	and n_name = ':3'
order by
	s_name;
:n -1
**************************/
select 
	s_name,
	s_address 
from 
	supplier, 
	nation 
where 
	s_suppkey in( 
		select 
			ps_suppkey 
		from 
			partsupp, 
			( 
				select 
					sum(l_quantity) as qty_sum, l_partkey, l_suppkey 
				from 
					lineitem 
				where 
					l_shipdate >= date ':2' 
					and l_shipdate < date ':2' + interval '1 year' 
				group by l_partkey, l_suppkey ) g 
		where 
			g.l_partkey = ps_partkey 
			and g.l_suppkey = ps_suppkey 
			and ps_availqty > 0.5 * g.qty_sum 
			and ps_partkey in ( select p_partkey from part where p_name like ':1%' ) 
		) 
	and s_nationkey = n_nationkey 
	and n_name = ':3'
order by 
	s_name;

