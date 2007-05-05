#!/bin/bash

rm -f sql/tpch/copytiny.sql

echo "copy region 	from '$PWD/data/tpch/tiny/region.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql 
echo "copy customer 		from '$PWD/data/tpch/tiny/customer.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy supplier 		from '$PWD/data/tpch/tiny/supplier.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy part 			from '$PWD/data/tpch/tiny/part.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy orders 		from '$PWD/data/tpch/tiny/orders.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy partsupp 		from '$PWD/data/tpch/tiny/partsupp.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy lineitem 		from '$PWD/data/tpch/tiny/lineitem.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
echo "copy nation 		from '$PWD/data/tpch/tiny/nation.tbl' with delimiter '|'; " >> sql/tpch/copytiny.sql
