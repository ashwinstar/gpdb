/*-------------------------------------------------------------------------
 *
 * gppc.c
 *	  libgppc wrapper main
 *
 * Copyright (c) 2012, Greenplum Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "funcapi.h"
#if GP_VERSION_NUM >= 40200
#include "tablefuncapi.h"
#endif

#include "mb/pg_wchar.h"
#include "parser/parse_expr.h"
#include "utils/builtins.h"

/*
 * GPPC_C_BUILD avoids definitions conflicting with actual backend definitions.
 */
#define GPPC_C_BUILD
#include "gppc.h"

#define ENSURE_NODE(val, nodetag) do{ \
	if (val != NULL && !IsA(val, nodetag)) \
		elog(ERROR, "unexpected value %d", (val == NULL ? 0 : nodeTag(val))); \
} while(0)

/*
 * libgppc.so has PG_MODULE_MAGIC, user libraries don't. Linking
 * to libgppc, user libraries can be loaded by the backend.
 */
PG_MODULE_MAGIC;

#define GPPC_MAP_FCINFO(info) ((FunctionCallInfo) info)

#define CHECK_TUPLEDESC_ATTNO(tupdesc, attno) do{ \
	if ((attno) < 0) \
		elog(ERROR, "requested invalid attno(%d)", (attno)); \
	if ((tupdesc) == NULL) \
		elog(ERROR, "tuple desc is null"); \
	if (((TupleDesc) (tupdesc))->natts <= (attno)) \
		elog(ERROR, "requested attno(%d) over tuple desc length(%d)", \
				(attno), ((TupleDesc) (tupdesc))->natts); \
}while(0)

/*
 * V1 call convension helper function.  This is just to avoid leaking
 * Pg_finfo_record struct.
 */
const void *
GppcFinfoV1(void)
{
	static const Pg_finfo_record my_finfo = { 1 };
	return &my_finfo;
}

/*
 * PG_NARGS
 */
int
GppcNargs(GppcFcinfo info)
{
	FunctionCallInfo	fcinfo = GPPC_MAP_FCINFO(info);

	return fcinfo->nargs;
}

/*
 * PG_RETURN_NULL
 */
GppcDatum
GppcReturnNull(GppcFcinfo info)
{
	FunctionCallInfo	fcinfo = GPPC_MAP_FCINFO(info);

	fcinfo->isnull = true;
	return (GppcDatum ) 0;
}

/*
 * PG_ARGISNULL
 */
bool
GppcArgIsNull(GppcFcinfo info, int n)
{
	FunctionCallInfo	fcinfo = GPPC_MAP_FCINFO(info);

	return PG_ARGISNULL(n);
}

/*
 * PG_GETARG_DATUM
 */
GppcDatum
GppcGetArgDatum(GppcFcinfo info, int n)
{
	FunctionCallInfo	fcinfo = GPPC_MAP_FCINFO(info);

	return PG_GETARG_DATUM(n);
}

/*
 * BoolGetDatum
 */
GppcDatum
GppcBoolGetDatum(GppcBool x)
{
	return BoolGetDatum(x);
}

/*
 * CharGetDatum
 */
GppcDatum
GppcCharGetDatum(GppcChar x)
{
	return CharGetDatum(x);
}

/*
 * Int16GetDatum
 */
GppcDatum
GppcInt2GetDatum(GppcInt2 x)
{
	return Int16GetDatum(x);
}

/*
 * Int32GetDatum
 */
GppcDatum
GppcInt4GetDatum(GppcInt4 x)
{
	return Int32GetDatum(x);
}

/*
 * Int64GetDatum
 */
GppcDatum
GppcInt8GetDatum(GppcInt8 x)
{
	return Int64GetDatum(x);
}

/*
 * Float4GetDatum
 */
GppcDatum
GppcFloat4GetDatum(GppcFloat4 x)
{
	return Float4GetDatum(x);
}

/*
 * Float8GetDatum
 */
GppcDatum
GppcFloat8GetDatum(GppcFloat8 x)
{
	return Float8GetDatum(x);
}

/*
 * TextGetDatum
 */
GppcDatum
GppcTextGetDatum(GppcText x)
{
	return PointerGetDatum(x);
}

/*
 * VarCharGetDatum
 */
GppcDatum
GppcVarCharGetDatum(GppcVarChar x)
{
	return PointerGetDatum(x);
}

/*
 * BpCharGetDatum
 */
GppcDatum
GppcBpCharGetDatum(GppcBpChar x)
{
	return PointerGetDatum(x);
}

/*
 * DatumGetBool
 */
GppcBool
GppcDatumGetBool(GppcDatum x)
{
	return DatumGetBool(x);
}

/*
 * DatumGetChar
 */
GppcChar
GppcDatumGetChar(GppcDatum x)
{
	return DatumGetChar(x);
}

/*
 * DatumGetInt16
 */
GppcInt2
GppcDatumGetInt2(GppcDatum x)
{
	return DatumGetInt16(x);
}

/*
 * DatumGetInt32
 */
GppcInt4
GppcDatumGetInt4(GppcDatum x)
{
	return DatumGetInt32(x);
}

/*
 * DatumGetInt64
 */
GppcInt8
GppcDatumGetInt8(GppcDatum x)
{
	return DatumGetInt64(x);
}

/*
 * DatumGetFloat4
 */
GppcFloat4
GppcDatumGetFloat4(GppcDatum x)
{
	return DatumGetFloat4(x);
}

/*
 * DatumGetFloat8
 */
GppcFloat8
GppcDatumGetFloat8(GppcDatum x)
{
	return DatumGetFloat8(x);
}

/*
 * DatumGetText
 */
const GppcText
GppcDatumGetText(GppcDatum x)
{
	return (GppcText) DatumGetTextP(x);
}

/*
 * DatumGetTextCopy
 */
GppcText
GppcDatumGetTextCopy(GppcDatum x)
{
	return (GppcText) DatumGetTextPCopy(x);
}

/*
 * DatumGetVarChar
 */
const GppcVarChar
GppcDatumGetVarChar(GppcDatum x)
{
	return (GppcVarChar) DatumGetVarCharP(x);
}

/*
 * DatumGetVarCharCopy
 */
GppcVarChar
GppcDatumGetVarCharCopy(GppcDatum x)
{
	return (GppcVarChar) DatumGetVarCharPCopy(x);
}

/*
 * DatumGetBpChar
 */
const GppcBpChar
GppcDatumGetBpChar(GppcDatum x)
{
	return (GppcBpChar) DatumGetBpCharP(x);
}

/*
 * DatumGetBpCharCopy
 */
GppcBpChar
GppcDatumGetBpCharCopy(GppcDatum x)
{
	return (GppcBpChar) DatumGetBpCharPCopy(x);
}

/*
 *
 */
size_t
GppcGetTextLength(GppcText t)
{
	return VARSIZE(t) - VARHDRSZ;
}

/*
 * VARDATA(text)
 */
char *
GppcGetTextPointer(GppcText t)
{
	return VARDATA(t);
}

/*
 * TextDatumGetCString
 */
char *
GppcTextGetCString(GppcText t)
{
	return TextDatumGetCString(PointerGetDatum(t));
}

/*
 * CStringGetTextDatum
 */
GppcText
GppcCStringGetText(const char *s)
{
	return (GppcText) cstring_to_text(s);
}

/*
 *
 */
size_t
GppcGetVarCharLength(GppcVarChar t)
{
	return VARSIZE(t) - VARHDRSZ;
}

/*
 * VARDATA(VarChar)
 */
char *
GppcGetVarCharPointer(GppcVarChar t)
{
	return VARDATA(t);
}

/*
 * VarCharDatumGetCString
 */
char *
GppcVarCharGetCString(GppcVarChar t)
{
	return TextDatumGetCString(PointerGetDatum(t));
}

/*
 * CStringGetVarCharDatum
 */
GppcVarChar
GppcCStringGetVarChar(const char *s)
{
	return (GppcVarChar) cstring_to_text(s);
}

/*
 *
 */
size_t
GppcGetBpCharLength(GppcBpChar t)
{
	return VARSIZE(t) - VARHDRSZ;
}

/*
 * VARDATA(BpChar)
 */
char *
GppcGetBpCharPointer(GppcBpChar t)
{
	return VARDATA(t);
}

/*
 * BpCharDatumGetCString
 */
char *
GppcBpCharGetCString(GppcBpChar t)
{
	return TextDatumGetCString(PointerGetDatum(t));
}

/*
 * CStringGetBpCharDatum
 */
GppcBpChar
GppcCStringGetBpChar(const char *s)
{
	return (GppcBpChar) cstring_to_text(s);
}

/*
 * palloc
 */
void *
GppcAlloc(size_t bytes)
{
	return palloc(bytes);
}

/*
 * palloc0
 */
void *
GppcAlloc0(size_t bytes)
{
	return palloc0(bytes);
}

/*
 * repalloc
 */
void *
GppcRealloc(void *ptr, size_t bytes)
{
	return repalloc(ptr, bytes);
}

/*
 * pfree
 */
void
GppcFree(void *ptr)
{
	pfree(ptr);
}

/*
 * clen is byte length of the content
 */
GppcText
GppcAllocText(size_t clen)
{
	text	   *t;
	size_t		bytes;

	bytes = VARHDRSZ + clen;
	t = (text *) palloc(bytes);
	SET_VARSIZE(t, bytes);
	return (GppcText) t;
}

/*
 * clen is byte length of the content
 */
GppcVarChar
GppcAllocVarChar(size_t clen)
{
	text	   *t;
	size_t		bytes;

	bytes = VARHDRSZ + clen;
	t = (text *) palloc(bytes);
	SET_VARSIZE(t, bytes);
	return (GppcVarChar) t;
}

/*
 * clen is byte length of the content
 */
GppcBpChar
GppcAllocBpChar(size_t clen)
{
	text	   *t;
	size_t		bytes;

	bytes = VARHDRSZ + clen;
	t = (text *) palloc(bytes);
	SET_VARSIZE(t, bytes);
	return (GppcBpChar) t;
}

/*
 * elog
 */
void
GppcReport(GppcReportLevel elevel, const char *fmt, ...)
{
	va_list		ap;
	char		buf[1024];

	/* TODO: elog doesn't accept va_list, but we need follow the behavior */
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	/* For some reason, we need the third argument to suppress compiler warning */
	elog(elevel, (const char *) buf, ' ');
}

/*
 * Check if database encoding is utf8
 */
bool
GppcIsDatabaseEncodingUtf8(void)
{
	return GetDatabaseEncoding() == PG_UTF8;
}

/*
 * pg_database_encoding_max_length
 */
int
GppcDatabaseEncodingMaxLength(void)
{
	return pg_database_encoding_max_length();
}
