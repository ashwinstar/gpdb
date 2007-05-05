SELECT * FROM sale, product WHERE sale.pn = product.pn and (sale.vn < 40 or sale.cn > 1);
