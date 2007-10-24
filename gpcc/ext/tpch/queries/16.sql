-- $ID$
-- TPC-H/TPC-R Parts/Supplier Relationship Query (Q16)
-- Functional Query Definition
-- Approved February 1998
:x
/********* Query 16 modified to remove NOT IN clause *********
:o
select
	p_brand,
	p_type,
	p_size,
	count(distinct ps_suppkey) as supplier_cnt
from
	partsupp,
	part
where
	p_partkey = ps_partkey
	and p_brand <> ':1'
	and p_type not like ':2%'
	and p_size in (:3, :4, :5, :6, :7, :8, :9, :10)
	and ps_suppkey not in (
		select
			s_suppkey
		from
			supplier
		where
			s_comment like '%Customer%Complaints%'
	)
group by
	p_brand,
	p_type,
	p_size
order by
	supplier_cnt desc,
	p_brand,
	p_type,
	p_size;
:n -1
*************/
select 
	p_brand, 
	p_type, 
	p_size, 
	count(distinct ps_suppkey) as supplier_cnt 
from 
	part, 
	partsupp left join supplier on (ps_suppkey=s_suppkey and s_comment like '%Customer%Complaints%' ) 
where 
	p_partkey = ps_partkey 
	and p_brand <> ':1' 
	and p_type not like ':2%' 
	and p_size in (:3, :4, :5, :6, :7, :8, :9, :10)
	and s_suppkey is null 
group by 
	p_brand, 
	p_type, 
	p_size 
order by 
	supplier_cnt desc, 
	p_brand, 
	p_type, 
	p_size
