select a,b,count(*),c from t1 group by rollup(a,b),c order by a,b,c;
