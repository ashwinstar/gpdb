-- regression test for the uuid datatype
-- creating test tables
CREATE TABLE guid1
(
 guid_field UUID,
 text_field TEXT DEFAULT(now())
);
CREATE TABLE guid2
(
    guid_field UUID,
    text_field TEXT DEFAULT(now())
);

-- inserting invalid data tests
-- too long
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-1111-111111111111F');
-- too short
INSERT INTO guid1(guid_field) VALUES('{11111111-1111-1111-1111-11111111111}');
-- valid data but invalid format
INSERT INTO guid1(guid_field) VALUES('111-11111-1111-1111-1111-111111111111');
INSERT INTO guid1(guid_field) VALUES('{22222222-2222-2222-2222-222222222222 ');
INSERT INTO guid1(guid_field) VALUES('{222-22222-2222-2222-2222-222222222222}');

-- invalid data
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-G111-111111111111');
INSERT INTO guid1(guid_field) VALUES('11+11111-1111-1111-1111-111111111111');
INSERT INTO guid1(guid_field) VALUES('11SF1111-1111-1111-1111-111111111XYZ');

--inserting three input formats
INSERT INTO guid1(guid_field) VALUES('111a111b-111C-111d-111e-111f111a111B');
INSERT INTO guid1(guid_field) VALUES('{22222222-2222-2222-2222-222222222222}');
INSERT INTO guid1(guid_field) VALUES('0123456789ABCDEF0123456789abcdef');

-- retrieving the inserted data
SELECT guid_field FROM guid1;

-- ordering test
SELECT guid_field FROM guid1 ORDER BY guid_field ASC;
SELECT guid_field FROM guid1 ORDER BY guid_field DESC;

-- = operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field = '111a111b-111C-111d-111E-111f111a111b';

-- <> operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field <> '111A111B111C111d111E111F111A111b';

-- < operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field < '22222222-2222-2222-2222-222222222222';

-- <= operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field <= '22222222-2222-2222-2222-222222222222';

-- > operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field > '22222222-2222-2222-2222-222222222222';

-- >= operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field >= '111a-111B111C-111d111e111F111a-111b';

-- btree and hash index creation test
CREATE INDEX guid1_btree ON guid1 USING BTREE (guid_field);

-- unique index test
CREATE UNIQUE INDEX guid1_unique_BTREE ON guid1 USING BTREE (guid_field);
-- should fail
INSERT INTO guid1(guid_field) VALUES('111a111B111C111d111e111F111a111b');

-- check to see whether the new indexes are actually there
SELECT count(*) FROM pg_class WHERE relkind='i' AND relname LIKE 'guid%';

-- populating the test tables with additional records & cast test
INSERT INTO guid1(guid_field) VALUES('44444444-4444-4444-4444-444444444444');
INSERT INTO guid2(guid_field) VALUES('11111111-1111-1111-1111-111111111111');
INSERT INTO guid2(guid_field) VALUES('{22222222-2222-2222-2222-222222222222}'::text);
INSERT INTO guid2(guid_field) VALUES('{0123-4567-89abcdef-0123456789abcdef}'::varchar);

SELECT guid_field::text from guid1;
SELECT guid_field::varchar from guid2;

-- join test
SELECT COUNT(*) FROM guid1 g1 INNER JOIN guid2 g2 ON g1.guid_field = g2.guid_field;
SELECT COUNT(*) FROM guid1 g1 LEFT JOIN guid2 g2 ON g1.guid_field = g2.guid_field WHERE g2.guid_field IS NULL;

-- clean up
DROP TABLE guid1, guid2 CASCADE;
