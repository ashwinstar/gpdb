\set TBL lineDelimOutTbl

CREATE OR REPLACE FUNCTION line_delim_out(record)
RETURNS bytea
AS '$libdir/line_delim.so', 'formatter_export'
LANGUAGE C STABLE;

DROP EXTERNAL TABLE IF EXISTS :TBL;
CREATE WRITABLE EXTERNAL TABLE :TBL (num1 float8, num2 float8)
LOCATION ('gpfdist://localhost:8080/lineDelimOut.tbl')				-- replace "localhost" with your hostname
FORMAT 'CUSTOM' (FORMATTER='line_delim_out', line_delim='$$');

INSERT INTO :TBL SELECT * FROM lineDelimTbl;