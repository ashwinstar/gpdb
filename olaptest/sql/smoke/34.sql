SELECT cname, MAX(qty*prc) FROM sale, customer WHERE sale.cn = customer.cn GROUP BY cname;
