select j,k,count(distinct l) from test group by rollup(j,k) order by j,k;
