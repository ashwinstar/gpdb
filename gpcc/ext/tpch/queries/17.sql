-- $ID$
-- TPC-H/TPC-R Small-Quantity-Order Revenue Query (Q17)
-- Functional Query Definition
-- Approved February 1998
:x
/******** Query 17 modified to remove correlation *******
:o
select
	sum(l_extendedprice) / 7.0 as avg_yearly
from
	lineitem,
	part
where
	p_partkey = l_partkey
	and p_brand = ':1'
	and p_container = ':2'
	and l_quantity < (
		select
			0.2 * avg(l_quantity)
		from
			lineitem
		where
			l_partkey = p_partkey
	);
:n -1
*********************/
select
      sum(l_extendedprice) / 7.0 as avg_yearly
from
      lineitem,
      (
          select
              p_partkey as x_partkey,
              0.2 * avg(l_quantity) as x_avg_20
          from
              part,
              lineitem
          where
              p_partkey = l_partkey
              and p_brand = ':1'
              and p_container = ':2'
          group by
              p_partkey
      ) x
where
      x_partkey = l_partkey
and l_quantity < x_avg_20;
