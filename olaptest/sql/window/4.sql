select * from customer window w1 as (), w2 as (w1 partition by pn); -- fail! can't partition if referencing ordering window
