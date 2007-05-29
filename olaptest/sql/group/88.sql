select count(*),pn,dt from sale group by grouping sets ((pn,dt), (dt));
