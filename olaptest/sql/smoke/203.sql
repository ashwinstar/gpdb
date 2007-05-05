SELECT * FROM sale WHERE cn in (SELECT cn FROM sale) ORDER BY cn;
