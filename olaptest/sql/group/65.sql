select i,k,avg(l),n from test group by rollup(n),i,k order by i,k,n;
