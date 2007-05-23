--
-- TRANSACTIONS
--

--
-- We want to have an error between the point where all segments are prepared and our decision 
-- to write the Distributed Commit record.
--
CREATE TABLE distxact1 (a int);
BEGIN;
INSERT INTO distxact1 VALUES (1);
INSERT INTO distxact1 VALUES (2);
INSERT INTO distxact1 VALUES (3);
INSERT INTO distxact1 VALUES (4);
INSERT INTO distxact1 VALUES (5);
INSERT INTO distxact1 VALUES (6);
INSERT INTO distxact1 VALUES (7);
INSERT INTO distxact1 VALUES (8);
SET debug_abort_after_distributed_prepared = true;
COMMIT;
SELECT * FROM distxact1;
DROP TABLE distxact1;

--
-- We want to have an error during the prepare which will cause a Abort-Some-Prepared broadcast 
-- to cleanup.  Action = 2 is FAIL_BEGIN_COMMAND.  Protocol = 2 is PREPARE.
--
CREATE TABLE distxact2 (a int);
BEGIN;
INSERT INTO distxact2 VALUES (21);
INSERT INTO distxact2 VALUES (22);
INSERT INTO distxact2 VALUES (23);
INSERT INTO distxact2 VALUES (24);
INSERT INTO distxact2 VALUES (25);
INSERT INTO distxact2 VALUES (26);
INSERT INTO distxact2 VALUES (27);
INSERT INTO distxact2 VALUES (28);
SET debug_dtm_action = 2;
SET debug_dtm_action_protocol = 2;
COMMIT;
SELECT * FROM distxact2;
DROP TABLE distxact2;


--
-- We want to have an error during the commit prepared broadcast which will cause a
-- Retry-Commit-Prepared broadcast to cleanup.
-- Action = 2 is FAIL_BEGIN_COMMAND.  Protocol = 4 is COMMIT_PREPARED.
--
CREATE TABLE distxact3 (a int);
BEGIN;
INSERT INTO distxact3 VALUES (31);
INSERT INTO distxact3 VALUES (32);
INSERT INTO distxact3 VALUES (33);
INSERT INTO distxact3 VALUES (34);
INSERT INTO distxact3 VALUES (35);
INSERT INTO distxact3 VALUES (36);
INSERT INTO distxact3 VALUES (37);
INSERT INTO distxact3 VALUES (38);
SET debug_dtm_action = 2;
SET debug_dtm_action_protocol = 4;
COMMIT;
SELECT * FROM distxact3;
DROP TABLE distxact3;


--
-- VARIANT of we want to have an error between the point where all segments are prepared and our decision 
-- to write the Distributed Commit record.  Cause problem during Abort Prepared broadcast.  
-- Action = 2 is FAIL_BEGIN_COMMAND.  Protocol = 5 is ABORT_PREPARED.
--
CREATE TABLE distxact4 (a int);
BEGIN;
INSERT INTO distxact4 VALUES (41);
INSERT INTO distxact4 VALUES (42);
INSERT INTO distxact4 VALUES (43);
INSERT INTO distxact4 VALUES (44);
INSERT INTO distxact4 VALUES (45);
INSERT INTO distxact4 VALUES (46);
INSERT INTO distxact4 VALUES (47);
INSERT INTO distxact4 VALUES (48);
SET debug_abort_after_distributed_prepared = true;
SET debug_dtm_action = 2;
SET debug_dtm_action_protocol = 5;
COMMIT;
SELECT * FROM distxact4;
DROP TABLE distxact4;
