select dense_rank() over (order by pname, cname), cname, pname from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
