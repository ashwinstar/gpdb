select row_number() over (order by vn), vn, count(*) from sale group by vn;
