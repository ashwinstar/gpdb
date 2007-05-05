SELECT * FROM (SELECT * FROM product) r, (SELECT * FROM product) s WHERE r.pn=s.pn;
