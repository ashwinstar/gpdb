select row_number() over (order by cn), count(*) over (order by cn) from sale; -- same
