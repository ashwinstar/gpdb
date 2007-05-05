SELECT * FROM product p1 LEFT JOIN product p2 ON substr(p1.pname,4) = substr(p2.pname,4);
