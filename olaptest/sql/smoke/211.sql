SELECT * FROM sale WHERE pn in (SELECT pn FROM product);
