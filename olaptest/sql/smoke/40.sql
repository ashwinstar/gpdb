SELECT SUM(prc),SUM(prc::int2),SUM(prc::int4),SUM(prc::int8),SUM(prc::numeric),SUM(prc::real), SUM(CAST(prc as double precision)), SUM(NOW()-NOW()) FROM sale;
