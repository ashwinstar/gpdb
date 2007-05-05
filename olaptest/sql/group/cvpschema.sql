-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- cvpschema.sql
--
--     Create a VERY simple star schema for CDB2 testing.
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

-- Clean up from last time.

\echo
\echo Drop existing tables. (Ignore <tablename> does not exist errors.)
\echo

drop table sale cascade;
drop table product cascade;
drop table vendor cascade;
drop table customer cascade;
drop table util cascade;

\echo 
\echo Creating tables
\echo 

create table customer 
(
	cn int not null,
	cname text not null,
	cloc text,
	
	primary key (cn)
	
) distributed by (cn);

create table vendor 
(
	vn int not null,
	vname text not null,
	vloc text,
	
	primary key (vn)
	
) distributed by (vn);

create table product 
(
	pn int not null,
	pname text not null,
	pcolor text,
	
	primary key (pn)
	
) distributed by (pn);

create table sale
(
	cn int not null,
	vn int not null,
	pn int not null,
	dt date not null,
	qty int not null,
	prc float not null,
	
	primary key (cn, vn, pn)
	
) distributed by (cn,vn,pn);

create table util
(
	xn int not null,
	
	primary key (xn)
	
) distributed by (xn);


\echo 
\echo Granting access to PUBLIC
\echo

grant select on sale to public;
grant select on product to public;
grant select on vendor to public;
grant select on customer to public;
grant select on util to public;

\echo
\echo Done: cvpschema.sql
\echo