SELECT * FROM util WHERE xn in (SELECT pn FROM product) ORDER BY xn;
