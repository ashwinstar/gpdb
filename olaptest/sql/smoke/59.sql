SELECT * FROM (SELECT * FROM sale) r, (SELECT * FROM product) s WHERE r.pn=s.pn ORDER BY cn, r.pn;
