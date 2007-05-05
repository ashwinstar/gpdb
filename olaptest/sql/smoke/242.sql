SELECT * FROM sale join product ON sale.pn = product.pn WHERE sale.vn < 40 or ((sale.cn > 1 and product.pn>=200));
