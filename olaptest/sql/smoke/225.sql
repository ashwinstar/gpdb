SELECT cn, (SELECT COUNT(*) FROM product) FROM sale;
