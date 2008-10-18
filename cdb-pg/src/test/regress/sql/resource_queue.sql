-- SQL coverage of RESOURCE QUEUE

CREATE RESOURCE QUEUE regressq ACTIVE THRESHOLD 1;
SELECT * FROM pg_resqueue WHERE rsqname='regressq';
ALTER RESOURCE QUEUE regressq ACTIVE THRESHOLD 2 COST THRESHOLD 2000.00;
SELECT * FROM pg_resqueue WHERE rsqname='regressq';
ALTER RESOURCE QUEUE regressq COST THRESHOLD 3000.00 OVERCOMMIT;
SELECT * FROM pg_resqueue WHERE rsqname='regressq';
ALTER RESOURCE QUEUE regressq COST THRESHOLD 4e+3 NOOVERCOMMIT;
SELECT * FROM pg_resqueue WHERE rsqname='regressq';
DROP RESOURCE QUEUE regressq;
SELECT * FROM pg_resqueue WHERE rsqname='regressq';

-- negative

CREATE RESOURCE QUEUE regressq2;
CREATE RESOURCE QUEUE regressq2 ACTIVE THRESHOLD 2;
ALTER RESOURCE QUEUE regressq2 ACTIVE THRESHOLD -10;
ALTER RESOURCE QUEUE regressq2 COST THRESHOLD -1000.00;
DROP RESOURCE QUEUE regressq2;

-- SQL coverage of ROLE -> RESOURCE QUEUE

CREATE RESOURCE QUEUE reg_activeq ACTIVE THRESHOLD 2;
CREATE RESOURCE QUEUE reg_costq COST THRESHOLD 30000.00;
CREATE USER reg_u1 RESOURCE QUEUE reg_costq;
GRANT SELECT ON tenk1 TO reg_u1;
SELECT u.rolname, u.rolsuper, r.rsqname FROM pg_roles as u, pg_resqueue as r WHERE u.rolresqueue=r.oid and rolname='reg_u1';
ALTER USER reg_u1 RESOURCE QUEUE reg_activeq;
SELECT u.rolname, u.rolsuper, r.rsqname FROM pg_roles as u, pg_resqueue as r WHERE u.rolresqueue=r.oid and rolname='reg_u1';
CREATE USER reg_u2 RESOURCE QUEUE reg_activeq;
GRANT SELECT ON tenk1 TO reg_u2;
SELECT u.rolname, u.rolsuper, r.rsqname FROM pg_roles as u, pg_resqueue as r WHERE u.rolresqueue=r.oid and r.rsqname='reg_activeq';

-- negative

CREATE USER reg_u3 RESOURCE QUEUE bogusq;

-- feature must be on for tests to be valid
show resource_scheduler;

-- switch to a non privileged user for next tests
SET SESSION AUTHORIZATION reg_u1;

-- self deadlock (active queue threshold 2)
BEGIN;
DECLARE c1 CURSOR FOR SELECT 1 FROM tenk1;
DECLARE c2 CURSOR FOR SELECT 2 FROM tenk1;
DECLARE c3 CURSOR FOR SELECT 3 FROM tenk1; -- should detect deadlock
END;

-- track cursor open/close count (should not deadlock)
BEGIN;
DECLARE c1 CURSOR FOR SELECT 1 FROM tenk1;
CLOSE c1;
DECLARE c2 CURSOR FOR SELECT 2 FROM tenk1;
DECLARE c3 CURSOR FOR SELECT 3 FROM tenk1;
CLOSE c3;
DECLARE c4 CURSOR FOR SELECT 4 FROM tenk1;
FETCH c4;
END;

-- switch to a cost-limited queue
RESET SESSION AUTHORIZATION;
ALTER USER reg_u2 RESOURCE QUEUE reg_costq;
SET SESSION AUTHORIZATION reg_u2;
BEGIN;
DECLARE c1 CURSOR FOR SELECT * FROM tenk1;
SELECT rsqname, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
DECLARE c2 CURSOR FOR SELECT * FROM tenk1 a NATURAL JOIN tenk1 b;
SELECT rsqname, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
CLOSE c1;
CLOSE c2;
END;

-- rsqcostvalue should go back to 0 when queue is empty (MPP-3578)
SELECT * FROM pg_resqueue_status where rsqcostvalue != 0 or rsqcountvalue != 0 or rsqholders != 0; -- 0 rows expected

-- MPP-3796.  When a cursor exceeds the cost limit and the transaction is
-- aborted, resources which had already been granted to other cursors should
-- be released.  Here there are no other concurrent transactions sharing the
-- queue, so rsqcostvalue should go back to 0.
BEGIN;
DECLARE c1 CURSOR FOR SELECT * FROM tenk1;
SELECT rsqname, rsqcostlimit, rsqwaiters, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
DECLARE c2 CURSOR FOR SELECT * FROM tenk1;
SELECT rsqname, rsqcostlimit, rsqwaiters, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
DECLARE c3 CURSOR FOR SELECT * FROM tenk1 a, tenk1 b;
SELECT rsqname, rsqcostlimit, rsqwaiters, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
DECLARE c4 CURSOR FOR SELECT * FROM tenk1 a, tenk1 b, tenk1 c;
SELECT rsqname, rsqcostlimit, rsqwaiters, rsqholders FROM pg_resqueue_status WHERE rsqcostvalue > 0;
END;
SELECT * FROM pg_resqueue_status where rsqcostvalue != 0 or rsqcountvalue != 0 or rsqholders != 0; -- 0 rows expected

-- return to the super user
RESET SESSION AUTHORIZATION;

-- reset session to super user. make sure no longer queued
BEGIN;
DECLARE c1 CURSOR FOR SELECT 1 FROM tenk1;
DECLARE c2 CURSOR FOR SELECT 2 FROM tenk1;
DECLARE c3 CURSOR FOR SELECT 3 FROM tenk1; -- should not deadlock, we are SU.
END;

-- cleanup

DROP OWNED BY reg_u1, reg_u2 CASCADE;
DROP USER reg_u1;
DROP USER reg_u2;
DROP RESOURCE QUEUE reg_activeq;
DROP RESOURCE QUEUE reg_costq;
