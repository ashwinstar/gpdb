select a,b,avg(d)+avg(c), avg(d)-avg(c) from t1 group by rollup(a,b);
