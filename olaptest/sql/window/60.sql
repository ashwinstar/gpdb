select pn, row_number() over (partition by pn), avg(pn) over (partition by pn) from sale order by pn;
