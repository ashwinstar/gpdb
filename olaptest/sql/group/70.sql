select j,k, grouping(j,k) from test group by rollup(j,k) order by k,j;
