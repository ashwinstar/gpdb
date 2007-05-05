SELECT * FROM sale join product USING(pn) WHERE sale.vn < 40 or ((sale.cn > 1 and product.pn>=200));
