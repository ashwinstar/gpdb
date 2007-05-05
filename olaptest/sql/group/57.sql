select b,c,count(distinct a),d from t1 group by rollup(b,c),d order by d,b,c;
