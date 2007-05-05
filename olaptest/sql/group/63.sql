select k,i,avg(l),j from test group by rollup(j,k),i order by k,i,j;select n, count(distinct l) from test group by rollup(n) order by n;
