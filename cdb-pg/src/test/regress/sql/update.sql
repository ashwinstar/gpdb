--
-- UPDATE ... SET <col> = DEFAULT;
--

CREATE TABLE update_test (
	c   INT DEFAULT 1,
    a   INT DEFAULT 10,
    b   INT
);

INSERT INTO update_test(a,b) VALUES (5, 10);
INSERT INTO update_test(a,b) VALUES (10, 15);

SELECT a,b FROM update_test ORDER BY a,b;

UPDATE update_test SET a = DEFAULT, b = DEFAULT;

SELECT a,b FROM update_test ORDER BY a,b;

DROP TABLE update_test;
