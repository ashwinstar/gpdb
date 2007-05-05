select b,c, avg(a), d from t1 group by rollup(b,c),d order by b,c,d;
