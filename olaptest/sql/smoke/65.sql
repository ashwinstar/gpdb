SELECT * FROM (SELECT * FROM product) r, (SELECT * FROM sale) s WHERE r.pn=s.cn;
