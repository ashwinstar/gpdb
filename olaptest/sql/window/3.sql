select row_number() over (w), * from sale order by 1 desc window w as (); -- fail! window can't follow order by
