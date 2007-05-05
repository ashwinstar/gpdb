select j,avg(l),group_id() as gid from test group by rollup(j),j order by j,gid;
