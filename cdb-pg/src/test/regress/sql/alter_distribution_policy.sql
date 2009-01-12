-- ALTER TABLE ... SET DISTRIBUTED BY
-- This is the main interface for system expansion

-- Basic sanity tests
create table atsdb (i int, j text) distributed by (i);

-- should fail
alter table atsdb set distributed by ();
alter table atsdb set distributed by (m);
alter table atsdb set distributed by (i, i);

-- should work
alter table atsdb set distributed randomly;
\d atsdb
alter table atsdb set distributed by (j);

drop table atsdb;
