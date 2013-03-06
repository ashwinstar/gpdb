create table orders
(
 order_id int,
 customer_id integer, 
 order_datetime timestamp, 
 order_total numeric(10,2)
);

insert into orders(customer_id, order_datetime, order_total) values 
    (1,'2009-05-01 10:00 AM', 500),
    (1,'2009-05-15 11:00 AM', 650),
    (2,'2009-05-11 11:00 PM', 100),
    (2,'2009-05-12 11:00 PM', 5),
    (3,'2009-04-11 11:00 PM', 100),
    (1,'2009-05-20 11:00 AM', 3);
