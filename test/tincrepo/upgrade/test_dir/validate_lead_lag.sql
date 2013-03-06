select order_id, customer_id, order_total, lead(order_total, 2) over (order by order_total) from orders;
select order_id, customer_id, order_total, lag(order_total, 2) over (order by order_total) from orders;
