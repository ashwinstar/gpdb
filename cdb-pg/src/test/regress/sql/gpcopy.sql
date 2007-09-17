--
-- This test suite tests COPY code that is unique to greenplum db. 
--

-- ######################################################
-- different distribution columns and distribution types
-- ######################################################

CREATE TABLE copy_regression_hash1(a text, b text, c text) distributed by (b);
CREATE TABLE copy_regression_hash2(a text, b text, c text) distributed by (a,c);
CREATE TABLE copy_regression_hash3(a varchar, b int, c int, d text) distributed by (a,c,d);
CREATE TABLE copy_regression_hash4(a int[], b text) distributed by (a);
CREATE TABLE copy_regression_hash5(a text[][]) distributed by (a);
CREATE TABLE copy_regression_hash6(a int[], b text[]) distributed by (a,b);
CREATE TABLE copy_regression_hash7(a text,b text) distributed randomly;

-- single key, not first

COPY copy_regression_hash1 from stdin;
a data	b data	c data
\.
COPY copy_regression_hash1(b,c,a) from stdin;
b data	c data	a data
\.
SELECT * FROM copy_regression_hash1;

-- two keys

COPY copy_regression_hash2 from stdin;
a data	b data	c data
\.
COPY copy_regression_hash2(b,c,a) from stdin;
b data	c data	a data
\.
SELECT * FROM copy_regression_hash2;

-- three keys

COPY copy_regression_hash3 from stdin;
a data	2	3	d data
\.
COPY copy_regression_hash3(c,d,b,a) from stdin;
3	d data	2	a data
\.
COPY copy_regression_hash3(a,c) from stdin;
a data	3
\.
COPY copy_regression_hash3(d) from stdin;
d data
\.
SELECT * FROM copy_regression_hash3;

-- hash on int array

COPY copy_regression_hash4 from stdin;
{1,2,3,4,5}	hashed on an integer array
{10,20,30,40,50}	hashed on an integer array
\.
SELECT * FROM copy_regression_hash4 ORDER BY a;

-- hash on 2 dim varchar array

COPY copy_regression_hash5 from stdin;
{{hashing,on},{two dimentional,text array}}
\.
SELECT * FROM copy_regression_hash5 ORDER BY a;

-- hash on int array and varchar array

COPY copy_regression_hash6 from stdin;
{1,2,3,4,5}	{hashing,on,intarray,and,varchararray}
\.
SELECT * FROM copy_regression_hash6 ORDER BY a;

-- hash randomly

COPY copy_regression_hash7 from stdin;
a data	b data
a data	b data
\.
SELECT * FROM copy_regression_hash7;

DROP TABLE copy_regression_hash1;
DROP TABLE copy_regression_hash2;
DROP TABLE copy_regression_hash3;
DROP TABLE copy_regression_hash4;
DROP TABLE copy_regression_hash5;
DROP TABLE copy_regression_hash6;
DROP TABLE copy_regression_hash7;

-- ######################################################
-- default values and default functions
-- ######################################################

CREATE TABLE copy_regression_default1(a text not null default 'a default data', b text not null default 'b default data') distributed by(a);
CREATE TABLE copy_regression_default2(a text, b serial, c text) distributed by(a);
CREATE TABLE copy_regression_default3(a serial, b text, c text) distributed by(a);

-- constant defaults on key and non key attributes

COPY copy_regression_default1(a) from stdin;
a data
\.
COPY copy_regression_default1(b) from stdin;
b data
\.
SELECT * FROM copy_regression_default1;

-- non constant default on non hash key in both text and csv

COPY copy_regression_default2(a,c) from stdin;
a data	c data
a data	c data 
a data	c data
\.
COPY copy_regression_default2(a,c) from stdin csv;
a data,c data
a data,c data
a data,c data
\.
SELECT * FROM copy_regression_default2;

-- non constant default on hash key in both text and csv

COPY copy_regression_default3(b,c) from stdin;
b data	c data
b data	c data
b data	c data
\.
COPY copy_regression_default3(b,c) from stdin csv;
b data,c data
b data,c data
b data,c data
\.
SELECT * FROM copy_regression_default2;

DROP TABLE copy_regression_default1;
DROP TABLE copy_regression_default2;
DROP TABLE copy_regression_default3;

-- ######################################################
-- COPY with OIDS
-- ######################################################

CREATE TABLE copy_regression_oids(a text) WITH OIDS;

COPY copy_regression_oids from stdin with oids delimiter '|';
50000|a text data
50001|a text data
\.
COPY copy_regression_oids from stdin with oids csv;
60000,a text data
60001,a text data
\.
SELECT * FROM copy_regression_oids ORDER BY oid;

DROP TABLE copy_regression_oids;



-- ######################################################
-- COPY OUT
-- ######################################################

CREATE TABLE copy_regression_out1(a text, b varchar, c int[], d bool) distributed by(a);

-- populating table with data for copy out tests. NOTE: since we can't control the order
-- of rows COPY OUT produces from the segdb, we must have only one row for each test table.

COPY copy_regression_out1 from stdin null 'nullval';
a copyout data line 2	nullval	{2,2,2}	true
\.

-- copy out default parameters text format..
COPY copy_regression_out1 to stdout;

-- copy out '|' delimiter 'mynull' null..
COPY copy_regression_out1 to stdout with delimiter '|' null 'mynull';

-- copy out '|' delimiter "i'm null" null..
COPY copy_regression_out1 to stdout with delimiter '|' null 'i''m null';

-- copy out default parameters csv format..
COPY copy_regression_out1 to stdout with csv;

-- copy out default parameters csv format with header..
COPY copy_regression_out1 to stdout with csv header;

-- copy out partial and mixed column list (c,a,b)..
COPY copy_regression_out1(c,a,b) to stdout;

-- copy out csv default quotes and escapes and special cases..
COPY copy_regression_out1 to stdout with csv quote ''''; -- escape should be the same as quote
COPY copy_regression_out1 to stdout with csv escape '''';
COPY copy_regression_out1 to stdout with csv quote '''' escape E'\\'; -- make sure E'' is accepted

DROP TABLE copy_regression_out1;


-- ######################################################
-- Zero column table
-- ######################################################

CREATE TABLE copy_regression_nocol();

-- copy in and out of zero column table..
COPY copy_regression_nocol from stdin;
\.
COPY copy_regression_nocol from stdin;
we should get an "extra data" error here
\.
COPY copy_regression_nocol from stdin with csv;
\.
COPY copy_regression_nocol from stdin with csv; -- should fail
we should get an "extra data" error here
\.
COPY copy_regression_nocol to stdout;
COPY copy_regression_nocol to stdout with csv;

DROP TABLE copy_regression_nocol;


-- ######################################################
-- Various text format escape and default format tests
-- ######################################################

-- for text format
CREATE TABLE copy_regression_text1(a varchar ,b varchar, c text, d text, e text) distributed by(a);
CREATE TABLE copy_regression_text2(a varchar ,b varchar, c text, d text, e text) distributed by(a);
CREATE TABLE copy_regression_text3(a varchar ,b varchar, c text, d text, e text) distributed by(a);

-- column order tests

COPY copy_regression_text1 from stdin; 
a text data	b text data	c text data	d text data	e text data
\.
COPY copy_regression_text1(a,b,c,d,e) from stdin;
a text data	b text data	c text data	d text data	e text data
\.
COPY copy_regression_text1(e,d,c,b,a) from stdin;
e text data	d text data	c text data	b text data	a text data
\.
COPY copy_regression_text1(c,a,b,e,d) from stdin;
c text data	a text data	b text data	e text data	d text data
\.
COPY copy_regression_text1(a,c) from stdin;
a text data	c text data
\.
SELECT * FROM copy_regression_text1;

-- null print tests

COPY copy_regression_text2 from stdin;
a text data	\N	c text data	\N	e text data
\.
COPY copy_regression_text2 from stdin with null 'nullvalue';
a text data	nullvalue	c text data	nullvalue	nullvalue
\.
COPY copy_regression_text2 from stdin with delimiter '|' null '';
a text data||c text data||e text data
|b text data|c text data||e text data
\.
COPY copy_regression_text2 from stdin with delimiter '|' null 'i''m null';
a text data|b text data|i'm null|i'm null|e text data
\.
SELECT * FROM copy_regression_text2;

-- escape tests

COPY copy_regression_text3 from stdin with delimiter '|' escape '#';
the at sign: #100 |b text data|c text data|d text data|e text data 
an embedded delimiter #| character |b text data|c text data|d text data|e text data
a single backslash \ in col a|b text data|c text data|d text data|e text data
\.
COPY copy_regression_text3 from stdin with delimiter '|' escape 'off';
a single backslash \ in col a|b text data|c text data|d text data|e text data
c:\\file\data\neew\path|b text data|c text data|d text data|e text data
\.
COPY copy_regression_text3 from stdin with delimiter '|';
the at sign: \100|b text data|c text data|d text data|e text data
an embedded linefeed \
and another one \
in column a|b text data|c text data|d text data|e text data
a single backslash \\ in col a|b text data|c text data|d text data|e text data
\.
SELECT * FROM copy_regression_text3 ORDER BY a;

DROP TABLE copy_regression_text1;
DROP TABLE copy_regression_text2;
DROP TABLE copy_regression_text3;



-- ######################################################
-- Various text format escape and default format tests
-- ######################################################

-- for csv format
CREATE TABLE copy_regression_csv1(a varchar ,b varchar, c text, d text, e text) distributed by(a);
CREATE TABLE copy_regression_csv2(a varchar ,b varchar, c text, d text, e text) distributed by(a);
CREATE TABLE copy_regression_csv3(a varchar ,b varchar, c text, d text, e text) distributed by(a);

-- column order tests

COPY copy_regression_csv1 from stdin with csv; 
a  csv data,b  csv data,c  csv data,d  csv data,e  csv data
\.
COPY copy_regression_csv1(a,b,c,d,e) from stdin with csv;
a  csv data,b  csv data,c  csv data,d  csv data,e csv  data
\.
COPY copy_regression_csv1(e,d,c,b,a) from stdin with csv;
e  csv data,d  csv data,c  csv data,b  csv data,a  csv data
\.
COPY copy_regression_csv1(c,a,b,e,d) from stdin with csv;
c  csv data,a  csv data,b  csv data,e  csv data,d  csv data
\.
COPY copy_regression_csv1(a,c) from stdin with csv;
a  csv data,c  csv data
\.
SELECT * FROM copy_regression_csv1;

-- null print tests

COPY copy_regression_csv2 from stdin with null E'\\N' csv ;
a  csv data,\N,c  csv data,\N,e  csv data
\.
COPY copy_regression_csv2 from stdin with null 'nullvalue' csv;
a  csv data,nullvalue,c  csv data,nullvalue,nullvalue
\.
COPY copy_regression_csv2 from stdin with delimiter '|' null '' csv;
a  csv data||c  csv data||e  csv data
|b  csv data|c  csv data||e  csv data
\.
SELECT * FROM copy_regression_csv2;

-- escape tests

COPY copy_regression_csv3 from stdin with csv escape E'\\';
"an embedded delimiter (comma), is printed",b  csv data,c  csv data,d  csv data,e  csv data 
"an embedded quote (doubleq)\" is printed",b  csv data,c  csv data,d  csv data,e  csv data 
"an embedded escape \\ is printed",b  csv data,c  csv data,d  csv data,e  csv data
"an embedded line feed
is printed",b  csv data,c  csv data,d  csv data,e  csv data
\.
COPY copy_regression_csv3 from stdin with delimiter E'\t' csv; --default quote and escape - "
"an embedded delimiter (tab)	 is printed"	b  csv data	c  csv data	d  csv data	e  csv data 
"an embedded quote or escape (doubleq)"" is printed"	b  csv data	c  csv data	d  csv data	e  csv data 
"an embedded line feed
is printed"	b  csv data	c  csv data	d  csv data	e  csv data
\.
COPY copy_regression_csv3 from stdin with delimiter '|' csv quote ';' escape '*';
;an embedded delimiter (pipe)| is printed;|b  csv data|c  csv data|d  csv data|e  csv data 
;an embedded quote (semicolon)*; is printed;|b  csv data|c  csv data|d  csv data|e  csv data 
;an embedded escape (asterisk)** is printed;|b  csv data|c  csv data|d  csv data|e  csv data
;an embedded line feed
is printed;|b  csv data|c  csv data|d  csv data|e  csv data
\.

-- check defaults
COPY copy_regression_csv3 from stdin with csv quote ''''; -- escape should be the same as quote
'an embedded single quote '' here',b csv data,c csv data,d csv data,e csv data 
\.
COPY copy_regression_csv3 from stdin with csv escape '''';
"an embedded single quote '' here",b csv data,c csv data,d csv data,e csv data 
\.
COPY copy_regression_csv3 from stdin with csv quote '''' escape E'\\'; -- make sure E'' is accepted
'an embedded backslash \\ here',b csv data,c csv data,d csv data,e csv data 
\.
SELECT * FROM copy_regression_csv3 ORDER BY a;

DROP TABLE copy_regression_csv1;
DROP TABLE copy_regression_csv2;
DROP TABLE copy_regression_csv3;


-- ###########################################################
-- distributed data error consolidation + original row numbers
-- ###########################################################

CREATE TABLE copy_regression_error1(a int, b int) distributed by(a);

-- parse error on QE (extra column on line 6)

COPY copy_regression_error1 from stdin;
1	1
2	2
3	3
4	4
5	5
6	6	6
7	7
\.

-- parse error on QD (missing column on line 3)
COPY copy_regression_error1 from stdin;
1	1
2	2
3
4	4
\.

-- convert error on QD (invalid type line 2)

COPY copy_regression_error1 from stdin;
1	1
two	2
3	3
\.

-- convert error on QE (invalid type line 5)
COPY copy_regression_error1 from stdin;
1	1
2	2
3	3
4	4
5	five
6	6
7	7
\.

DROP TABLE copy_regression_error1;

-- ###########################################################
-- Single row error handling - SEGMENT REJECT LIMIT tests
-- ###########################################################

CREATE TABLE copy_regression_reject1(a int, b int, c int) distributed by(a);

-- test various errors that would get discovered on QD and QE COPY
-- only 3 rows here are good data, 5 should get rejected.
COPY copy_regression_reject1 FROM STDIN SEGMENT REJECT LIMIT 10;
1	1	1
bad data
3	3	3
4	4	bad attribute
5
6	6	6	6
7	7	7
8
\.

SELECT * FROM copy_regression_reject1 ORDER BY a; -- should have 3 rows
TRUNCATE copy_regression_reject1;

-- the very low reject limit should get reached here
COPY copy_regression_reject1 FROM STDIN SEGMENT REJECT LIMIT 2;
1	1	1
bad data
3	3	3
4	4	bad attribute
5
6	6	6	6
7	7	7
8
\.

SELECT * FROM copy_regression_reject1 ORDER BY a; -- should have no rows!

-- invalid reject limit, must be larger than 1
COPY copy_regression_reject1 FROM STDIN SEGMENT REJECT LIMIT 1;
\.

DROP TABLE copy_regression_reject1;










