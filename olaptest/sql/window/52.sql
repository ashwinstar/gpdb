select cn, row_number() over (w), count(*) over (w), rank() over (w), dense_rank() over (w), vn from sale window w as (partition by cn order by vn);
