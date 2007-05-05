select rank() over (w), cn, pn from sale window w as (order by pn desc);
