select row_number() over (), vn, count(*), sum(qty*prc) as amt from sale group by vn;
