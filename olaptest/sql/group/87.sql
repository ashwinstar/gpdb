select cn+vn,pn,count(*) from sale group by rollup(cn+vn,pn);
