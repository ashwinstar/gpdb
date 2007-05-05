select row_number() over (), s.pn, s.cn, s.vn, s.dt, s.qty, s.prc, c.cname, c.cloc, p.pname, p.pcolor from sale s, customer c, product p where s.cn = c.cn and s.pn = p.pn;
