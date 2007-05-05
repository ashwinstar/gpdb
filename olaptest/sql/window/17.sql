select row_number() over (partition by vn), vn, count(*) from sale group by vn;
