select cn,vn,count(*) from sale group by grouping sets((vn), (cn,vn));
