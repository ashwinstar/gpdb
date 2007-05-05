select grouping(cn), grouping(vn), grouping(pn), cn, vn, pn, count(*) from sale group by rollup(cn,vn,pn);
