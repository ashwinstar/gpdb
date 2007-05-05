select * from customer window w1 as (order by a rows unbounded preceding), w2 as (w1); -- fail! ordering window can't specify framing
