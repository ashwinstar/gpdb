select row_number() over (w), rank() over (w), dense_rank() over (w), * from sale window w as (order by cn);
