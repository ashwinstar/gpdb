select i,k, count(*) from test group by rollup(i,k) order by i,k;
