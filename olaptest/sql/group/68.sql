select a,b,grouping(b),avg(c) from t3 group by rollup(a,b) order by a,b;
