select grouping(cn,vn,pn) from sale group by rollup(cn,vn,pn) order by 1 desc;
