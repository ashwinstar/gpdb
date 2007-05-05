select k,l,avg(i) from test group by rollup(l,k) order by k,l;
