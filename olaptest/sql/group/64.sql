select j,k,avg(l),n from test group by rollup(n,j,k) order by j,k,n;
