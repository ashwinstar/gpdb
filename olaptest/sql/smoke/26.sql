SELECT cname, COUNT(*) FROM sale, customer WHERE sale.cn = customer.cn GROUP BY cname;
