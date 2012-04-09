-- AK: taken from postgres 8.3 docs

\set ECHO all
\i fuzzystrmatch.sql

-- soundex

SELECT soundex('hello world!');

SELECT soundex('Anne'), soundex('Ann'), difference('Anne', 'Ann');
SELECT soundex('Anne'), soundex('Andrew'), difference('Anne', 'Andrew');
SELECT soundex('Anne'), soundex('Margaret'), difference('Anne', 'Margaret');

CREATE TABLE s (nm text);

INSERT INTO s VALUES ('john');
INSERT INTO s VALUES ('joan');
INSERT INTO s VALUES ('wobbly');
INSERT INTO s VALUES ('jack');

SELECT * FROM s WHERE soundex(nm) = soundex('john');

SELECT * FROM s WHERE difference(s.nm, 'john') > 2;

-- levenshtein

SELECT levenshtein('GUMBO', 'GAMBOL');

-- metaphone

SELECT metaphone('GUMBO', 4);

-- double metaphone

select dmetaphone('gumbo');
