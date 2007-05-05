select row_number() over (partition by pname, cname) from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
