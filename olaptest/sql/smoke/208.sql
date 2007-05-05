SELECT * FROM product WHERE pn in (SELECT xn FROM util);
