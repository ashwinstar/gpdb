SELECT cn, MIN(qty*prc) FROM sale GROUP BY cn ORDER BY cn;
