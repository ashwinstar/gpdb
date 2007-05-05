select row_number() over (order by count(*)), vn, count(*) from sale group by vn;
