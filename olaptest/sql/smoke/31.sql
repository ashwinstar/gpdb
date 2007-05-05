SELECT cname, COUNT(qty) FROM sale, customer WHERE sale.cn = customer.cn GROUP BY cname ORDER BY cname;
