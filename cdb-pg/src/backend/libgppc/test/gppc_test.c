/*-------------------------------------------------------------------------
 *
 * gppc_client.c
 *	  libgppc client program
 *
 * Copyright (c) 2012, Greenplum Inc.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "gppc.h"

GPPC_FUNCTION(boolfunc)
{
	GppcBool	arg1 = GPPC_GETARG_BOOL(0);

	GPPC_RETURN_BOOL(arg1);
}

GPPC_FUNCTION(charfunc)
{
	GppcChar	arg1 = GPPC_GETARG_CHAR(0);

	GPPC_RETURN_CHAR(arg1);
}

GPPC_FUNCTION(int2mulfunc)
{
	GppcInt2	arg1 = GPPC_GETARG_INT2(0);
	GppcInt2	arg2 = GPPC_GETARG_INT2(1);

	GPPC_RETURN_INT2(arg1 * arg2);
}

GPPC_FUNCTION(int4func1)
{
	int			nargs = GPPC_NARGS();
	GppcInt4	arg1;

	if (nargs < 1)
		arg1 = 0;
	else
		arg1 = GPPC_GETARG_INT4(0);

	GPPC_RETURN_INT4(arg1);
}

GPPC_FUNCTION(int8plusfunc)
{
	GppcInt8	arg1 = GPPC_GETARG_INT8(0);
	GppcInt8	arg2 = GPPC_GETARG_INT8(1);

	GPPC_RETURN_INT8(arg1 + arg2);
}

GPPC_FUNCTION(float4func1)
{
	GppcFloat4	arg1 = GPPC_GETARG_FLOAT4(0);

	GPPC_RETURN_FLOAT4(arg1 + 1.5);
}

GPPC_FUNCTION(float8func1)
{
	GppcFloat8	arg1 = GPPC_GETARG_FLOAT8(0);

	GPPC_RETURN_FLOAT8(arg1 / 2.0);
}

GPPC_FUNCTION(textdoublefunc)
{
	GppcText	arg1 = GPPC_GETARG_TEXT(0);
	size_t		clen = GppcGetTextLength(arg1);
	GppcText	result;

	result = GppcAllocText(clen * 2);
	memcpy(GppcGetTextPointer(result), GppcGetTextPointer(arg1), clen);
	memcpy(GppcGetTextPointer(result) + clen, GppcGetTextPointer(arg1), clen);

	GPPC_RETURN_TEXT(result);
}

GPPC_FUNCTION(textgenfunc)
{
	GppcText	result = GppcCStringGetText("cstring result");

	GPPC_RETURN_TEXT(result);
}

GPPC_FUNCTION(textcopyfunc)
{
	GppcText	copy = GPPC_GETARG_TEXT_COPY(0);
	GppcText	arg = GPPC_GETARG_TEXT(0);
	GppcBool	needcopy = GPPC_GETARG_BOOL(1);

	*(GppcGetTextPointer(copy)) = '!';

	GPPC_RETURN_TEXT(needcopy ? copy : arg);
}

GPPC_FUNCTION(varchardoublefunc)
{
	GppcVarChar	arg1 = GPPC_GETARG_VARCHAR(0);
	size_t		clen = GppcGetVarCharLength(arg1);
	GppcVarChar	result;

	result = GppcAllocVarChar(clen * 2);
	memcpy(GppcGetVarCharPointer(result), GppcGetVarCharPointer(arg1), clen);
	memcpy(GppcGetVarCharPointer(result) + clen, GppcGetVarCharPointer(arg1), clen);

	GPPC_RETURN_VARCHAR(result);
}

GPPC_FUNCTION(varchargenfunc)
{
	GppcVarChar	result = GppcCStringGetVarChar("cstring result");

	GPPC_RETURN_VARCHAR(result);
}

GPPC_FUNCTION(varcharcopyfunc)
{
	GppcVarChar	copy = GPPC_GETARG_VARCHAR_COPY(0);
	GppcVarChar	arg = GPPC_GETARG_VARCHAR(0);
	GppcBool	needcopy = GPPC_GETARG_BOOL(1);

	*(GppcGetVarCharPointer(copy)) = '!';

	GPPC_RETURN_VARCHAR(needcopy ? copy : arg);
}

GPPC_FUNCTION(bpchardoublefunc)
{
	GppcBpChar	arg1 = GPPC_GETARG_BPCHAR(0);
	size_t		clen = GppcGetBpCharLength(arg1);
	GppcBpChar	result;

	result = GppcAllocBpChar(clen * 2);
	memcpy(GppcGetBpCharPointer(result), GppcGetBpCharPointer(arg1), clen);
	memcpy(GppcGetBpCharPointer(result) + clen, GppcGetBpCharPointer(arg1), clen);

	GPPC_RETURN_BPCHAR(result);
}

GPPC_FUNCTION(bpchargenfunc)
{
	GppcBpChar	result = GppcCStringGetBpChar("cstring result");

	GPPC_RETURN_BPCHAR(result);
}

GPPC_FUNCTION(bpcharcopyfunc)
{
	GppcBpChar	copy = GPPC_GETARG_BPCHAR_COPY(0);
	GppcBpChar	arg = GPPC_GETARG_BPCHAR(0);
	GppcBool	needcopy = GPPC_GETARG_BOOL(1);

	*(GppcGetBpCharPointer(copy)) = '!';

	GPPC_RETURN_BPCHAR(needcopy ? copy : arg);
}

GPPC_FUNCTION(errfunc1)
{
	GppcText	arg1 = GPPC_GETARG_TEXT(0);
	char	   *cstr;

	cstr = GppcTextGetCString(arg1);
	GppcReport(GPPC_NOTICE, "%s", cstr);

	GPPC_RETURN_NULL();
}

GPPC_FUNCTION(argisnullfunc)
{
	if (GPPC_ARGISNULL(0))
		GPPC_RETURN_BOOL(true);
	else
		GPPC_RETURN_BOOL(false);
}
