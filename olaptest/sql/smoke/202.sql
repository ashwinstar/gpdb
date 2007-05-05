SELECT * FROM product WHERE pn in (SELECT pn FROM sale) ORDER BY pn;
