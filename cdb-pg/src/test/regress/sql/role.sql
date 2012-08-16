-- 
-- ROLE
--

-- MPP-15479: ALTER ROLE SET statement
DROP ROLE IF EXISTS role_112911;
CREATE ROLE role_112911 WITH LOGIN;
CREATE SCHEMA common_schema;

/* Alter Role Set statement_mem */
ALTER ROLE role_112911 SET statement_mem TO '150MB';
SELECT gp_segment_id, rolname, rolconfig
  FROM pg_authid WHERE rolname = 'role_112911'
 UNION ALL
SELECT DISTINCT 0 as gp_segment_id, rolname, rolconfig
  FROM gp_dist_random('pg_authid') WHERE rolname = 'role_112911';

/* Alter Role Set search_path */
ALTER ROLE role_112911 SET search_path = common_schema;
SELECT gp_segment_id, rolname, rolconfig
  FROM pg_authid WHERE rolname = 'role_112911'
 UNION ALL
SELECT DISTINCT 0 as gp_segment_id, rolname, rolconfig
  FROM gp_dist_random('pg_authid') WHERE rolname = 'role_112911';

/* Alter Role Reset statement_mem */
ALTER ROLE role_112911 RESET statement_mem;
SELECT gp_segment_id, rolname, rolconfig
  FROM pg_authid WHERE rolname = 'role_112911'
 UNION ALL
SELECT DISTINCT 0 as gp_segment_id, rolname, rolconfig
  FROM gp_dist_random('pg_authid') WHERE rolname = 'role_112911';

/* Alter Role Set statement_mem */
ALTER ROLE role_112911 SET statement_mem = 100000;
SELECT gp_segment_id, rolname, rolconfig
  FROM pg_authid WHERE rolname = 'role_112911'
 UNION ALL
SELECT DISTINCT 0 as gp_segment_id, rolname, rolconfig
  FROM gp_dist_random('pg_authid') WHERE rolname = 'role_112911';

/* Alter Role Reset All */
ALTER ROLE role_112911 RESET ALL;
SELECT gp_segment_id, rolname, rolconfig
  FROM pg_authid WHERE rolname = 'role_112911'
 UNION ALL
SELECT DISTINCT 0 as gp_segment_id, rolname, rolconfig
  FROM gp_dist_random('pg_authid') WHERE rolname = 'role_112911';

DROP ROLE role_112911;
DROP SCHEMA common_schema;

-- SHA-256 testing
set password_hash_algorithm to "SHA-256";
create role sha256 password 'abc';
-- MPP-15865
-- OpenSSL SHA2 returning a different SHA2 to RSA BSAFE!
--select rolname, rolpassword from pg_authid where rolname = 'sha256';
drop role sha256;

/* Create role createdb, createuser, createrole */
SET password_encryption=off;
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH LOGIN PASSWORD 'pass_18122' createrole createuser createdb;

SELECT DISTINCT 0 as gp_segment_id, rolname, rolpassword, rolcreaterole, rolcreatedb
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

/* Create Role nocreateuser, nocreatedb */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH LOGIN PASSWORD 'pass_18122' nocreateuser nocreatedb;

SELECT DISTINCT 0 as gp_segment_id, rolname, rolpassword, rolcreaterole, rolcreatedb
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

/* Create Role encrypted password */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH LOGIN ENCRYPTED PASSWORD 'pass_18122';

SELECT DISTINCT 0 as gp_segment_id, rolname 
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122' and rolpassword='pass_18122';

/* Create Role unencrypted password */
SET password_encryption=on;
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH LOGIN UNENCRYPTED PASSWORD 'pass_18122';

SELECT DISTINCT 0 as gp_segment_id, rolname, rolpassword
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

/* Create role password NULL */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH LOGIN PASSWORD NULL;

SELECT DISTINCT 0 as gp_segment_id, rolname, rolpassword
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

reset password_encryption;

/* Create role with superuser */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH SUPERUSER;

SELECT DISTINCT 0 as gp_segment_id, rolname, rolsuper
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

/* Create role with inherit */
DROP ROLE IF EXISTS role_18122_inherit;
CREATE ROLE role_18122_inherit WITH createrole createdb; 

create table test_18122(a int, b int);
grant insert on test_18122 to role_18122_inherit; 

DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH INHERIT IN ROLE role_18122_inherit; 

SET ROLE role_18122; 
INSERT INTO test_18122 values(1, 2);

RESET ROLE;

/* Ceate role with noinherit */
DROP ROLE IF EXISTS role_18122_1;
CREATE ROLE role_18122_1 WITH NOINHERIT IN ROLE role_18122_inherit; 

SET ROLE role_18122_1; 
INSERT INTO test_18122 values(1, 2);

RESET ROLE;

DROP TABLE test_18122;

DROP ROLE role_18122_1;
DROP ROLE role_18122;
DROP ROLE role_18122_inherit;

/* Create role with nologin */
DROP RESOURCE QUEUE rq_18122; 
CREATE RESOURCE QUEUE rq_18122 WITH (MEMORY_LIMIT='1GB', ACTIVE_STATEMENTS=4);

DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH NOLOGIN CONNECTION LIMIT 10 RESOURCE QUEUE rq_18122 valid until '2011-08-11';

SELECT DISTINCT 0 as gp_segment_id, rolname, rolconnlimit, rolvaliduntil
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

/* Create role with nocreaterole */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH NOLOGIN NOCREATEROLE; 

SET ROLE role_18122; 
CREATE ROLE another_role WITH NOLOGIN;

RESET ROLE;

/* Create role with admin name_list */
DROP ROLE IF EXISTS test_role;
CREATE ROLE test_role WITH NOLOGIN NOCREATEROLE; 

/* test_role is a member of role_18122 with admin privileges */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH ADMIN test_role;

select m.rolname as membername, g.rolname as groupname, admin_option 
from pg_auth_members, pg_roles m, pg_roles g 
where g.oid = roleid and m.oid=member;

ALTER GROUP role_18122 drop USER test_role;

select m.rolname as membername, g.rolname as groupname, admin_option 
from pg_auth_members, pg_roles m, pg_roles g 
where g.oid = roleid and m.oid=member;

/* Create role with role name_list */
DROP ROLE IF EXISTS test_role; 
CREATE ROLE test_role WITH NOLOGIN NOCREATEROLE; 

/* test_role is added as a member of role_18122 */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH ROLE test_role;

select m.rolname as membername, g.rolname as groupname, admin_option 
from pg_auth_members, pg_roles m, pg_roles g 
where g.oid = roleid and m.oid=member;

/* Create role with createexttable exttable_auth_list */
DROP ROLE IF EXISTS role_18122;
CREATE ROLE role_18122 WITH login createexttable (type='writable', protocol='gpfdist');

SELECT DISTINCT 0 as gp_segment_id, rolname, rolcreaterextgpfd, rolcreaterexthttp, 
rolcreatewextgpfd, rolcreaterexthdfs
  FROM gp_dist_random('pg_authid') WHERE rolname='role_18122';

DROP ROLE role_18122; 
DROP ROLE test_role; 
DROP RESOURCE QUEUE rq_18122; 

