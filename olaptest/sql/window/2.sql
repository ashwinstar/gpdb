select row_number() over (w), * from sale window w as () order by 1 desc;
