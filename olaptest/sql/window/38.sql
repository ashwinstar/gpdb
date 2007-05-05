select row_number() over (), a,b from (select cn, count(*) from sale group by cn) r(a,b);
