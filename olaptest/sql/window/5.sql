select * from customer window w1 as (partition by a order by b), w2 as (w1 order by c); -- fail! can't reorder the ordering window
