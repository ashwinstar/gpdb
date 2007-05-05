select a,sum(c),group_id() as gid from t1 group by rollup(a,a),a,a order by a,gid;
