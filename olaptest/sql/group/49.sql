select grouping(cn), grouping(vn), grouping(pn) from sale group by rollup(cn,vn,pn) order by 1 desc, 2 desc, 3 desc;
