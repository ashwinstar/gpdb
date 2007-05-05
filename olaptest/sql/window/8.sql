select * from customer window w as (partition by x), w as (w order by s); -- fail! can't refer to window in its definition
