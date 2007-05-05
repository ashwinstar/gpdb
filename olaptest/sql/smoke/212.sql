SELECT * FROM sale WHERE cn in (SELECT cn FROM sale);
