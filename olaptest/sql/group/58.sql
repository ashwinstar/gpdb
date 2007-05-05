select j,k, count(*) from test group by rollup(j,k) order by k,j;
