SELECT * FROM sale s1, sale s2 WHERE date_part('month',s1.dt) = date_part('day',s2.dt);
