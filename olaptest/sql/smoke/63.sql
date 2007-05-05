SELECT * FROM (SELECT * FROM product) r, (SELECT * FROM util) s WHERE r.pn=s.xn;
