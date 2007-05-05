select b,c,count(*),d from t1 group by rollup(b,c),d order by b,c,d;select b,c,count(*),d from t2 group by rollup(b,c),d;
