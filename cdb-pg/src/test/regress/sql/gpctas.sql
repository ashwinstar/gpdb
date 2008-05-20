drop table if exists ctas_src;
drop table if exists ctas_dst;

create table ctas_src (domain integer, class integer, attr text, value integer) distributed by (domain);
insert into ctas_src values(1, 1, 'A', 1);
insert into ctas_src values(2, 1, 'A', 0);
insert into ctas_src values(3, 0, 'B', 1);

-- MPP-2859
create table ctas_dst as 
SELECT attr, class, (select count(distinct class) from ctas_src) as dclass FROM ctas_src GROUP BY attr, class distributed by (attr);

drop table ctas_dst;

create table ctas_dst as 
SELECT attr, class, (select max(class) from ctas_src) as maxclass FROM ctas_src GROUP BY attr, class distributed by (attr);

drop table ctas_dst;

create table ctas_dst as 
SELECT attr, class, (select count(distinct class) from ctas_src) as dclass, (select max(class) from ctas_src) as maxclass, (select min(class) from ctas_src) as minclass FROM ctas_src GROUP BY attr, class distributed by (attr);


