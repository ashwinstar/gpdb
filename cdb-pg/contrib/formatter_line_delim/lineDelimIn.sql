\set TBL lineDelimTbl

CREATE OR REPLACE FUNCTION line_delim_in()
RETURNS record
AS '$libdir/line_delim.so', 'formatter_import'
LANGUAGE C STABLE;

DROP EXTERNAL TABLE IF EXISTS :TBL;
CREATE READABLE EXTERNAL TABLE :TBL (num1 float8, num2 float8)
LOCATION ('gpfdist://localhost:8080/lineDelimIn.tbl')	-- replace "localhost" with your hostname
FORMAT 'CUSTOM' (FORMATTER='line_delim_in', line_delim='$$')
LOG ERRORS INTO err_line_delim SEGMENT REJECT LIMIT 10 ROWS;

SELECT * FROM :TBL;