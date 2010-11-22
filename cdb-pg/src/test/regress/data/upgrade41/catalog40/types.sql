-- Fill in type shells for types defined in the 4.0 Catalog:

-- Handle small number of composite types by hand
CREATE TYPE nb_classification AS (classes text[], accum float8[], apriori bigint[]);

-- Derived via the following SQL run within a 4.0 catalog
/*
SELECT 'CREATE TYPE ' || quote_ident(t.typname)
    || '('
    || E'\n  INPUT = upg_catalog.' || pin.proname
    || E',\n  OUTPUT = upg_catalog.' || pout.proname
    || case when precv.proname is not null 
            then E',\n  RECEIVE = upg_catalog.' || precv.proname
            else '' end
    || case when psend.proname is not null 
            then E',\n  SEND = upg_catalog.' || psend.proname
            else '' end
    || case when panalyze.proname is not null 
            then E',\n  ANALYZE = upg_catalog.' || panalyze.proname
            else '' end
    || case when t.typlen < 0 
            then E',\n  INTERNALLENGTH = VARIABLE'
            else E',\n  INTERNALLENGTH = ' || t.typlen end
    || case when t.typbyval 
            then E',\n  PASSEDBYVALUE' else '' end
    || E',\n  STORAGE = ' 
    || case when t.typstorage = 'p' then 'plain'
            when t.typstorage = 'x' then 'extended'
            when t.typstorage = 'm' then 'main'
            when t.typstorage = 'e' then 'external'
            else 'BROKEN' end
    || case when t.typdefault is not null 
            then E',\n  DEFAULT = ' || t.typdefault else '' end
    || case when telement.typname is not null
            then E',\n  ELEMENT = ' || telement.typname else '' end
    || E',\n  DELIMITER = '','''
    || E'\n);'
FROM pg_type t
     join pg_namespace n on (t.typnamespace = n.oid)
     join pg_proc pin on (t.typinput = pin.oid)
     join pg_proc pout on (t.typoutput = pout.oid)
     left join pg_proc precv on (t.typreceive = precv.oid)
     left join pg_proc psend on (t.typsend = psend.oid)
     left join pg_proc panalyze on (t.typanalyze = panalyze.oid)
     left join pg_type telement on (t.typelem = telement.oid)
WHERE t.typtype in ('b', 'p') and t.typname !~ '^_' and n.nspname = 'pg_catalog'
order by 1
;
*/
 CREATE TYPE "any"(
   INPUT = upg_catalog.any_in,
   OUTPUT = upg_catalog.any_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE "bit"(
   INPUT = upg_catalog.bit_in,
   OUTPUT = upg_catalog.bit_out,
   RECEIVE = upg_catalog.bit_recv,
   SEND = upg_catalog.bit_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE "char"(
   INPUT = upg_catalog.charin,
   OUTPUT = upg_catalog.charout,
   RECEIVE = upg_catalog.charrecv,
   SEND = upg_catalog.charsend,
   INTERNALLENGTH = 1,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE "interval"(
   INPUT = upg_catalog.interval_in,
   OUTPUT = upg_catalog.interval_out,
   RECEIVE = upg_catalog.interval_recv,
   SEND = upg_catalog.interval_send,
   INTERNALLENGTH = 16,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE "numeric"(
   INPUT = upg_catalog.numeric_in,
   OUTPUT = upg_catalog.numeric_out,
   RECEIVE = upg_catalog.numeric_recv,
   SEND = upg_catalog.numeric_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = main,
   DELIMITER = ','
 );
 CREATE TYPE "time"(
   INPUT = upg_catalog.time_in,
   OUTPUT = upg_catalog.time_out,
   RECEIVE = upg_catalog.time_recv,
   SEND = upg_catalog.time_send,
   INTERNALLENGTH = 8,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE "timestamp"(
   INPUT = upg_catalog.timestamp_in,
   OUTPUT = upg_catalog.timestamp_out,
   RECEIVE = upg_catalog.timestamp_recv,
   SEND = upg_catalog.timestamp_send,
   INTERNALLENGTH = 8,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE "varchar"(
   INPUT = upg_catalog.varcharin,
   OUTPUT = upg_catalog.varcharout,
   RECEIVE = upg_catalog.varcharrecv,
   SEND = upg_catalog.varcharsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE abstime(
   INPUT = upg_catalog.abstimein,
   OUTPUT = upg_catalog.abstimeout,
   RECEIVE = upg_catalog.abstimerecv,
   SEND = upg_catalog.abstimesend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE aclitem(
   INPUT = upg_catalog.aclitemin,
   OUTPUT = upg_catalog.aclitemout,
   INTERNALLENGTH = 12,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE anyarray(
   INPUT = upg_catalog.anyarray_in,
   OUTPUT = upg_catalog.anyarray_out,
   RECEIVE = upg_catalog.anyarray_recv,
   SEND = upg_catalog.anyarray_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE anyelement(
   INPUT = upg_catalog.anyelement_in,
   OUTPUT = upg_catalog.anyelement_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE bool(
   INPUT = upg_catalog.boolin,
   OUTPUT = upg_catalog.boolout,
   RECEIVE = upg_catalog.boolrecv,
   SEND = upg_catalog.boolsend,
   INTERNALLENGTH = 1,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE box(
   INPUT = upg_catalog.box_in,
   OUTPUT = upg_catalog.box_out,
   RECEIVE = upg_catalog.box_recv,
   SEND = upg_catalog.box_send,
   INTERNALLENGTH = 32,
   STORAGE = plain,
   ELEMENT = point,
   DELIMITER = ','
 );
 CREATE TYPE bpchar(
   INPUT = upg_catalog.bpcharin,
   OUTPUT = upg_catalog.bpcharout,
   RECEIVE = upg_catalog.bpcharrecv,
   SEND = upg_catalog.bpcharsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE bytea(
   INPUT = upg_catalog.byteain,
   OUTPUT = upg_catalog.byteaout,
   RECEIVE = upg_catalog.bytearecv,
   SEND = upg_catalog.byteasend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE cid(
   INPUT = upg_catalog.cidin,
   OUTPUT = upg_catalog.cidout,
   RECEIVE = upg_catalog.cidrecv,
   SEND = upg_catalog.cidsend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE cidr(
   INPUT = upg_catalog.cidr_in,
   OUTPUT = upg_catalog.cidr_out,
   RECEIVE = upg_catalog.cidr_recv,
   SEND = upg_catalog.cidr_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE circle(
   INPUT = upg_catalog.circle_in,
   OUTPUT = upg_catalog.circle_out,
   RECEIVE = upg_catalog.circle_recv,
   SEND = upg_catalog.circle_send,
   INTERNALLENGTH = 24,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE cstring(
   INPUT = upg_catalog.cstring_in,
   OUTPUT = upg_catalog.cstring_out,
   RECEIVE = upg_catalog.cstring_recv,
   SEND = upg_catalog.cstring_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE date(
   INPUT = upg_catalog.date_in,
   OUTPUT = upg_catalog.date_out,
   RECEIVE = upg_catalog.date_recv,
   SEND = upg_catalog.date_send,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE float4(
   INPUT = upg_catalog.float4in,
   OUTPUT = upg_catalog.float4out,
   RECEIVE = upg_catalog.float4recv,
   SEND = upg_catalog.float4send,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE float8(
   INPUT = upg_catalog.float8in,
   OUTPUT = upg_catalog.float8out,
   RECEIVE = upg_catalog.float8recv,
   SEND = upg_catalog.float8send,
   INTERNALLENGTH = 8,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE gpaotid(
   INPUT = upg_catalog.gpaotidin,
   OUTPUT = upg_catalog.gpaotidout,
   RECEIVE = upg_catalog.gpaotidrecv,
   SEND = upg_catalog.gpaotidsend,
   INTERNALLENGTH = 6,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE gpxlogloc(
   INPUT = upg_catalog.gpxloglocin,
   OUTPUT = upg_catalog.gpxloglocout,
   RECEIVE = upg_catalog.gpxloglocrecv,
   SEND = upg_catalog.gpxloglocsend,
   INTERNALLENGTH = 8,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE inet(
   INPUT = upg_catalog.inet_in,
   OUTPUT = upg_catalog.inet_out,
   RECEIVE = upg_catalog.inet_recv,
   SEND = upg_catalog.inet_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE int2(
   INPUT = upg_catalog.int2in,
   OUTPUT = upg_catalog.int2out,
   RECEIVE = upg_catalog.int2recv,
   SEND = upg_catalog.int2send,
   INTERNALLENGTH = 2,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE int2vector(
   INPUT = upg_catalog.int2vectorin,
   OUTPUT = upg_catalog.int2vectorout,
   RECEIVE = upg_catalog.int2vectorrecv,
   SEND = upg_catalog.int2vectorsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   ELEMENT = int2,
   DELIMITER = ','
 );
 CREATE TYPE int4(
   INPUT = upg_catalog.int4in,
   OUTPUT = upg_catalog.int4out,
   RECEIVE = upg_catalog.int4recv,
   SEND = upg_catalog.int4send,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE int8(
   INPUT = upg_catalog.int8in,
   OUTPUT = upg_catalog.int8out,
   RECEIVE = upg_catalog.int8recv,
   SEND = upg_catalog.int8send,
   INTERNALLENGTH = 8,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE internal(
   INPUT = upg_catalog.internal_in,
   OUTPUT = upg_catalog.internal_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE language_handler(
   INPUT = upg_catalog.language_handler_in,
   OUTPUT = upg_catalog.language_handler_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE line(
   INPUT = upg_catalog.line_in,
   OUTPUT = upg_catalog.line_out,
   RECEIVE = upg_catalog.line_recv,
   SEND = upg_catalog.line_send,
   INTERNALLENGTH = 32,
   STORAGE = plain,
   ELEMENT = float8,
   DELIMITER = ','
 );
 CREATE TYPE lseg(
   INPUT = upg_catalog.lseg_in,
   OUTPUT = upg_catalog.lseg_out,
   RECEIVE = upg_catalog.lseg_recv,
   SEND = upg_catalog.lseg_send,
   INTERNALLENGTH = 32,
   STORAGE = plain,
   ELEMENT = point,
   DELIMITER = ','
 );
 CREATE TYPE macaddr(
   INPUT = upg_catalog.macaddr_in,
   OUTPUT = upg_catalog.macaddr_out,
   RECEIVE = upg_catalog.macaddr_recv,
   SEND = upg_catalog.macaddr_send,
   INTERNALLENGTH = 6,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE money(
   INPUT = upg_catalog.cash_in,
   OUTPUT = upg_catalog.cash_out,
   RECEIVE = upg_catalog.cash_recv,
   SEND = upg_catalog.cash_send,
   INTERNALLENGTH = 4,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE name(
   INPUT = upg_catalog.namein,
   OUTPUT = upg_catalog.nameout,
   RECEIVE = upg_catalog.namerecv,
   SEND = upg_catalog.namesend,
   INTERNALLENGTH = 64,
   STORAGE = plain,
   ELEMENT = char,
   DELIMITER = ','
 );
 CREATE TYPE oid(
   INPUT = upg_catalog.oidin,
   OUTPUT = upg_catalog.oidout,
   RECEIVE = upg_catalog.oidrecv,
   SEND = upg_catalog.oidsend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE oidvector(
   INPUT = upg_catalog.oidvectorin,
   OUTPUT = upg_catalog.oidvectorout,
   RECEIVE = upg_catalog.oidvectorrecv,
   SEND = upg_catalog.oidvectorsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   ELEMENT = oid,
   DELIMITER = ','
 );
 CREATE TYPE opaque(
   INPUT = upg_catalog.opaque_in,
   OUTPUT = upg_catalog.opaque_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE path(
   INPUT = upg_catalog.path_in,
   OUTPUT = upg_catalog.path_out,
   RECEIVE = upg_catalog.path_recv,
   SEND = upg_catalog.path_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE point(
   INPUT = upg_catalog.point_in,
   OUTPUT = upg_catalog.point_out,
   RECEIVE = upg_catalog.point_recv,
   SEND = upg_catalog.point_send,
   INTERNALLENGTH = 16,
   STORAGE = plain,
   ELEMENT = float8,
   DELIMITER = ','
 );
 CREATE TYPE polygon(
   INPUT = upg_catalog.poly_in,
   OUTPUT = upg_catalog.poly_out,
   RECEIVE = upg_catalog.poly_recv,
   SEND = upg_catalog.poly_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE record(
   INPUT = upg_catalog.record_in,
   OUTPUT = upg_catalog.record_out,
   RECEIVE = upg_catalog.record_recv,
   SEND = upg_catalog.record_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE refcursor(
   INPUT = upg_catalog.textin,
   OUTPUT = upg_catalog.textout,
   RECEIVE = upg_catalog.textrecv,
   SEND = upg_catalog.textsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE regclass(
   INPUT = upg_catalog.regclassin,
   OUTPUT = upg_catalog.regclassout,
   RECEIVE = upg_catalog.regclassrecv,
   SEND = upg_catalog.regclasssend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE regoper(
   INPUT = upg_catalog.regoperin,
   OUTPUT = upg_catalog.regoperout,
   RECEIVE = upg_catalog.regoperrecv,
   SEND = upg_catalog.regopersend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE regoperator(
   INPUT = upg_catalog.regoperatorin,
   OUTPUT = upg_catalog.regoperatorout,
   RECEIVE = upg_catalog.regoperatorrecv,
   SEND = upg_catalog.regoperatorsend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE regproc(
   INPUT = upg_catalog.regprocin,
   OUTPUT = upg_catalog.regprocout,
   RECEIVE = upg_catalog.regprocrecv,
   SEND = upg_catalog.regprocsend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE regprocedure(
   INPUT = upg_catalog.regprocedurein,
   OUTPUT = upg_catalog.regprocedureout,
   RECEIVE = upg_catalog.regprocedurerecv,
   SEND = upg_catalog.regproceduresend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE regtype(
   INPUT = upg_catalog.regtypein,
   OUTPUT = upg_catalog.regtypeout,
   RECEIVE = upg_catalog.regtyperecv,
   SEND = upg_catalog.regtypesend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE reltime(
   INPUT = upg_catalog.reltimein,
   OUTPUT = upg_catalog.reltimeout,
   RECEIVE = upg_catalog.reltimerecv,
   SEND = upg_catalog.reltimesend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE smgr(
   INPUT = upg_catalog.smgrin,
   OUTPUT = upg_catalog.smgrout,
   INTERNALLENGTH = 2,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE text(
   INPUT = upg_catalog.textin,
   OUTPUT = upg_catalog.textout,
   RECEIVE = upg_catalog.textrecv,
   SEND = upg_catalog.textsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE tid(
   INPUT = upg_catalog.tidin,
   OUTPUT = upg_catalog.tidout,
   RECEIVE = upg_catalog.tidrecv,
   SEND = upg_catalog.tidsend,
   INTERNALLENGTH = 6,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE timestamptz(
   INPUT = upg_catalog.timestamptz_in,
   OUTPUT = upg_catalog.timestamptz_out,
   RECEIVE = upg_catalog.timestamptz_recv,
   SEND = upg_catalog.timestamptz_send,
   INTERNALLENGTH = 8,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE timetz(
   INPUT = upg_catalog.timetz_in,
   OUTPUT = upg_catalog.timetz_out,
   RECEIVE = upg_catalog.timetz_recv,
   SEND = upg_catalog.timetz_send,
   INTERNALLENGTH = 12,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE tinterval(
   INPUT = upg_catalog.tintervalin,
   OUTPUT = upg_catalog.tintervalout,
   RECEIVE = upg_catalog.tintervalrecv,
   SEND = upg_catalog.tintervalsend,
   INTERNALLENGTH = 12,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE trigger(
   INPUT = upg_catalog.trigger_in,
   OUTPUT = upg_catalog.trigger_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE unknown(
   INPUT = upg_catalog.unknownin,
   OUTPUT = upg_catalog.unknownout,
   RECEIVE = upg_catalog.unknownrecv,
   SEND = upg_catalog.unknownsend,
   INTERNALLENGTH = VARIABLE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE varbit(
   INPUT = upg_catalog.varbit_in,
   OUTPUT = upg_catalog.varbit_out,
   RECEIVE = upg_catalog.varbit_recv,
   SEND = upg_catalog.varbit_send,
   INTERNALLENGTH = VARIABLE,
   STORAGE = extended,
   DELIMITER = ','
 );
 CREATE TYPE void(
   INPUT = upg_catalog.void_in,
   OUTPUT = upg_catalog.void_out,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );
 CREATE TYPE xid(
   INPUT = upg_catalog.xidin,
   OUTPUT = upg_catalog.xidout,
   RECEIVE = upg_catalog.xidrecv,
   SEND = upg_catalog.xidsend,
   INTERNALLENGTH = 4,
   PASSEDBYVALUE,
   STORAGE = plain,
   DELIMITER = ','
 );

