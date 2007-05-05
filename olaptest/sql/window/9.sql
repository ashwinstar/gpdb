select * from customer window w as (w order by s); -- fail! can't refer to window in its definition 
