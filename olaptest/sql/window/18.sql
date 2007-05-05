select row_number() over (partition by count(*)), vn, count(*) from sale group by vn;
