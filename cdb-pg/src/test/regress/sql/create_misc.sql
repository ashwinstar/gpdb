--
-- CREATE_MISC
--

-- CLASS POPULATION
--	(any resemblance to real life is purely coincidental)
--

INSERT INTO tenk2 SELECT * FROM tenk1;

SELECT * INTO TABLE onek2 FROM onek;

INSERT INTO fast_emp4000 SELECT * FROM slow_emp4000;

SELECT *
   INTO TABLE Bprime
   FROM tenk1
   WHERE unique2 < 1000;

INSERT INTO hobbies_r (name, person)
   SELECT 'posthacking', p.name
   FROM person* p
   WHERE p.name = 'mike' or p.name = 'jeff';

INSERT INTO hobbies_r (name, person)
   SELECT 'basketball', p.name
   FROM person p
   WHERE p.name = 'joe' or p.name = 'sally';

INSERT INTO hobbies_r (name) VALUES ('skywalking');

INSERT INTO equipment_r (name, hobby) VALUES ('advil', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('peet''s coffee', 'posthacking');

INSERT INTO equipment_r (name, hobby) VALUES ('hightops', 'basketball');

INSERT INTO equipment_r (name, hobby) VALUES ('guts', 'skywalking');

SELECT *
   INTO TABLE ramp
   FROM road
   WHERE name ~ '.*Ramp';

--
-- for internal portal (cursor) tests
--
CREATE TABLE iportaltest (
	i		int4, 
	d		float4, 
	p		polygon
);

INSERT INTO iportaltest (i, d, p)
   VALUES (1, 3.567, '(3.0,1.0),(4.0,2.0)'::polygon);

INSERT INTO iportaltest (i, d, p)
   VALUES (2, 89.05, '(4.0,2.0),(3.0,1.0)'::polygon);

-- check cdb-hashable type vs CREATE TABLE
CREATE TABLE point_dist(p point) DISTRIBUTED BY (p);
CREATE TABLE pointarr_dist(pa point[]) DISTRIBUTED BY (pa);
CREATE DOMAIN pd AS point[];
CREATE TABLE pd_dist(pa pd) DISTRIBUTED BY (pa);
DROP TABLE pointarr_dist;
DROP TABLE pd_dist;
CREATE TABLE pointarr_dist(pa point[]);
INSERT INTO pointarr_dist VALUES(ARRAY[point(1,1)]);
CREATE TABLE pd_dist(pa pd);
INSERT INTO pd_dist VALUES(ARRAY[point(1,1)]);
