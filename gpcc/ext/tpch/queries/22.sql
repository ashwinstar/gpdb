-- $ID$
-- TPC-H/TPC-R Global Sales Opportunity Query (Q22)
-- Functional Query Definition
-- Approved February 1998
:x
/********* Query 22 modified to remove correlation ******
:o
select
	cntrycode,
	count(*) as numcust,
	sum(c_acctbal) as totacctbal
from
	(
		select
			substring(c_phone from 1 for 2) as cntrycode,
			c_acctbal
		from
			customer
		where
			substring(c_phone from 1 for 2) in
				(':1', ':2', ':3', ':4', ':5', ':6', ':7')
			and c_acctbal > (
				select
					avg(c_acctbal)
				from
					customer
				where
					c_acctbal > 0.00
					and substring(c_phone from 1 for 2) in
						(':1', ':2', ':3', ':4', ':5', ':6', ':7')
			)
			and not exists (
				select
					*
				from
					orders
				where
					o_custkey = c_custkey
			)
	) as custsale
group by
	cntrycode
order by
	cntrycode;
:n -1
***********************************/
select 
	cntrycode, 
	count(*) as numcust, 
	sum(c_acctbal) as totacctbal 
from 
	( 
		select 
			substring(c_phone from 1 for 2) as cntrycode, 
			c_acctbal 
		from 
			customer left join orders 
				on c_custkey = o_custkey 
		where 
			substring(c_phone from 1 for 2) in 
				(':1', ':2', ':3', ':4', ':5', ':6', ':7') 
			and c_acctbal > ( 
				select 
					avg(c_acctbal) 
				from 
					customer 
				where 
					c_acctbal > 0.00 
					and substring(c_phone from 1 for 2) in 
						(':1', ':2', ':3', ':4', ':5', ':6', ':7') 
			) 
			and o_custkey is null 
	) as custsale 
group by 
	cntrycode 
order by 
	cntrycode
