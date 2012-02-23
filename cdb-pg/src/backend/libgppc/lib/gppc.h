/* ----------------------------------------------------------------------- *//**
 *
 * @file gppc.h
 *
 * @brief Greenplum Partner Connector main header
 *
 * This header contains prototypes for libgppc -- the Greenplum Database Partner
 * Connector.
 *
 * The most common use of the GPPC is to write user defined functions which do
 * not need to be recompiled from one Greenplum Database release to the next.
 *
 * Those user defined functions usually have the following form:
 *
 * \code
 *   GPPC_FUNCTION_INFO(funcname);
 *   
 *   GppcDatum
 *   funcname(GPPC_FUNCTION_ARGS)
 *   {
 *     GppcXXX arg1 = GPPC_GETARG_XXXX(0);
 *     ....
 *     GPPC_RETURN_XXX(result);
 *   }
 * \endcode
 *
 * Copyright (c) 2012, Greenplum Inc.
 *
 *//* ----------------------------------------------------------------------- */
#ifndef GPPC_H
#define GPPC_H

#include <sys/types.h>
#include <stdint.h>

#include "gppc_config.h"

#ifndef GPPC_C_BUILD

#ifdef PG_VERSION_NUM
#error "PG_VERSION_NUM detected. Please not include GPDB internal headers."
#endif

#ifndef __cplusplus /* C++ has built-in bool */

/**
 * \brief Generic boolean type if it's not defined.
 */
typedef char		bool;
#ifndef true
/**
 * \brief Generic boolean true if it's not defined.
 */
#define true ((bool) 1)
#endif
#ifndef false
/**
 * \brief Generic boolean false if it's not defined.
 */
#define false ((bool) 0)
#endif

#endif  /* __cplusplus */

#endif  /* GPPC_C_BUILD */

/**
 * \brief Creates the necessary glue to make the named function SQL invokable.
 * \param funcname the name of exposed function.
 *
 * Every SQL invokable function must be declared via this macro.
 */
#define GPPC_FUNCTION_INFO(funcname) \
extern const void * pg_finfo_##funcname (void); \
const void * \
pg_finfo_##funcname (void) \
{ \
	return GppcFinfoV1(); \
} \
extern int no_such_variable

/**
 * \brief SQL callable function's argument.
 *
 * Every function must have the same signature with this macro
 * as its only argument.
 */
#define GPPC_FUNCTION_ARGS GppcFcinfo fcinfo

/****************************************************************************
 * GPPC types
 ****************************************************************************/
/**
 * \brief A type which contains function call information.
 *
 * The contents of this data type are opaque to users of GPPC.
 */
typedef struct GppcFcinfoData		   *GppcFcinfo;

/**
 * \brief Generic type which can represent any data type.
 */
typedef int64_t							GppcDatum;

/**
 * \brief One byte boolean type which maps to SQL bool.
 */
typedef char							GppcBool;

/**
 * \brief One byte character type which maps to SQL "char".
 */
typedef char							GppcChar;

/**
 * \brief Two byte signed integer type which maps to SQL int2/smallint.
 */
typedef int16_t							GppcInt2;

/**
 * \brief Four byte signed integer type which maps to SQL int4/integer.
 */
typedef int32_t							GppcInt4;

/**
 * \brief Eight byte signed integer type which maps to SQL int8/bigint.
 */
typedef int64_t							GppcInt8;

/**
 * \brief Four byte floating point type which maps to SQL float4/real.
 */
typedef float							GppcFloat4;

/**
 * \brief Eight byte floating point type which maps to SQL
 * float8/double precision.
 */
typedef double							GppcFloat8;

/**
 * \brief Generic string type which maps to SQL text.
 */
typedef struct GppcTextData			   *GppcText;

/**
 * \brief Variable length string which maps to SQL varchar.
 */
typedef struct GppcVarCharData		   *GppcVarChar;

/**
 * \brief Blank padded string which maps to SQL char.
 */
typedef struct GppcBpCharData		   *GppcBpChar;

/**
 * \brief Returns number of argument supplied to an SQL invoked function.
 */
#define GPPC_NARGS() (GppcNargs(fcinfo))

/**
 * \brief Terminates an SQL invoked function returning NULL value.
 */
#define GPPC_RETURN_NULL() return GppcReturnNull(fcinfo)

/**
 * \brief Returns true if a given argument to an SQL invoked function was NULL.
 * \param n the argument position starting from 0.
 */
#define GPPC_ARGISNULL(n) (GppcArgIsNull(fcinfo, n))

/****************************************************************************
 * GETARG macros
 ****************************************************************************/
/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcBool
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_BOOL(n)		(GppcDatumGetBool(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcChar
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_CHAR(n)		(GppcDatumGetChar(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcInt2
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT2(n)		(GppcDatumGetInt2(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcInt4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT4(n)		(GppcDatumGetInt4(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcInt4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT8(n)		(GppcDatumGetInt8(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcFloat4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_FLOAT4(n)	(GppcDatumGetFloat4(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcFloat8
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_FLOAT8(n)	(GppcDatumGetFloat8(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcText
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcText may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_TEXT(n)		(GppcDatumGetText(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcText
 * \param n the argument position starting from 0.
 *
 * In contrast to GPPC_GETARG_TEXT(), this returns a copy of the argument,
 * so the function can modify its content.
 */
#define GPPC_GETARG_TEXT_COPY(n)	(GppcDatumGetTextCopy(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcVarChar
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcVarChar may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_VARCHAR(n)	(GppcDatumGetVarChar(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcVarChar
 * \param n the argument position starting from 0.
 *
 * In contrast to GPPC_GETARG_VARCHAR(), this returns a copy of the argument,
 * so the function can modify its content.
 */
#define GPPC_GETARG_VARCHAR_COPY(n)	(GppcDatumGetVarCharCopy(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcBpChar
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcBpChar may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_BPCHAR(n)	(GppcDatumGetBpChar(GppcGetArgDatum(fcinfo, n)))

/**
 * \brief Retrieves an argument to an SQL invoked function as ::GppcBpChar
 * \param n the argument position starting from 0.
 *
 * In contrast to GPPC_GETARG_BPCHAR(), this returns a copy of the argument,
 * so the function can modify its content.
 */
#define GPPC_GETARG_BPCHAR_COPY(n)	(GppcDatumGetBpCharCopy(GppcGetArgDatum(fcinfo, n)))

/****************************************************************************
 * RETURN macros
 ****************************************************************************/
/**
 * \brief Terminates an SQL invoked function returning value as SQL bool.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_BOOL(x)		return GppcBoolGetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL "char".
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_CHAR(x)		return GppcCharGetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL int2.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT2(x)		return GppcInt2GetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL int4.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT4(x)		return GppcInt4GetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL int8.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT8(x)		return GppcInt8GetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL float4.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_FLOAT4(x)	return GppcFloat4GetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL float8.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_FLOAT8(x)	return GppcFloat8GetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL text.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_TEXT(x)		return GppcTextGetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL varchar.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_VARCHAR(x)	return GppcVarCharGetDatum(x)

/**
 * \brief Terminates an SQL invoked function returning value as SQL char.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_BPCHAR(x)	return GppcBpCharGetDatum(x)

/**
 * \sa GPPC_FUNCTION_INFO()
 */
const void *GppcFinfoV1(void);

/**
 * \sa GPPC_NARGS()
 */
int GppcNargs(GppcFcinfo info);

/**
 * \sa GPPC_RETURN_NULL()
 */
GppcDatum GppcReturnNull(GppcFcinfo info);

/**
 * \sa GPPC_ARGISNULL()
 */
bool GppcArgIsNull(GppcFcinfo info, int n);

/**
 * \sa GPPC_GETARG_INT4(), etc.
 */
GppcDatum GppcGetArgDatum(GppcFcinfo info, int n);

/****************************************************************************
 * Conversion to GppcDatum
 ****************************************************************************/
/**
 * \brief Converts ::GppcBool to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcBool
 */
GppcDatum		GppcBoolGetDatum(GppcBool x);

/**
 * \brief Converts ::GppcChar to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcChar
 */
GppcDatum		GppcCharGetDatum(GppcChar x);

/**
 * \brief Converts ::GppcInt2 to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcInt2
 */
GppcDatum		GppcInt2GetDatum(GppcInt2 x);

/**
 * \brief Converts ::GppcInt4 to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcInt4
 */
GppcDatum		GppcInt4GetDatum(GppcInt4 x);

/**
 * \brief Converts ::GppcInt8 to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcInt8
 */
GppcDatum		GppcInt8GetDatum(GppcInt8 x);

/**
 * \brief Converts ::GppcFloat4 to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcFloat4
 */
GppcDatum		GppcFloat4GetDatum(GppcFloat4 x);

/**
 * \brief Converts ::GppcFloat8 to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcFloat8
 */
GppcDatum		GppcFloat8GetDatum(GppcFloat8 x);

/**
 * \brief Converts ::GppcText to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcText
 */
GppcDatum		GppcTextGetDatum(GppcText x);

/**
 * \brief Converts ::GppcVarChar to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcVarChar
 */
GppcDatum		GppcVarCharGetDatum(GppcVarChar x);

/**
 * \brief Converts ::GppcBpChar to ::GppcDatum
 * \param x ::GppcDatum to be converted to ::GppcBpChar
 */
GppcDatum		GppcBpCharGetDatum(GppcBpChar x);

/****************************************************************************
 * Conversion from GppcDatum
 ****************************************************************************/
/**
 * \brief Converts ::GppcDatum to ::GppcBool
 * \param x ::GppcDatum to be converted to ::GppcBool
 */
GppcBool		GppcDatumGetBool(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcChar
 * \param x ::GppcDatum to be converted to ::GppcChar
 */
GppcChar		GppcDatumGetChar(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcInt2
 * \param x ::GppcDatum to be converted to ::GppcInt2
 */
GppcInt2		GppcDatumGetInt2(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcInt4
 * \param x ::GppcDatum to be converted to ::GppcInt4
 */
GppcInt4		GppcDatumGetInt4(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcInt8
 * \param x ::GppcDatum to be converted to ::GppcInt8
 */
GppcInt8		GppcDatumGetInt8(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcFloat4
 * \param x ::GppcDatum to be converted to ::GppcFloat4
 */
GppcFloat4		GppcDatumGetFloat4(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcFloat8
 * \param x ::GppcDatum to be converted to ::GppcFloat8
 */
GppcFloat8		GppcDatumGetFloat8(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcText
 * \param x ::GppcDatum to be converted to ::GppcText
 */
GppcText		GppcDatumGetText(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcText
 * \param x ::GppcDatum to be converted and copied to ::GppcText
 */
GppcText		GppcDatumGetTextCopy(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcVarChar
 * \param x ::GppcDatum to be converted to ::GppcVarChar
 */
GppcVarChar		GppcDatumGetVarChar(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcVarChar
 * \param x ::GppcDatum to be converted and copied to ::GppcVarChar
 */
GppcVarChar		GppcDatumGetVarCharCopy(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcBpChar
 * \param x ::GppcDatum to be converted to ::GppcBpChar
 */
GppcBpChar		GppcDatumGetBpChar(GppcDatum x);

/**
 * \brief Converts ::GppcDatum to ::GppcBpChar
 * \param x ::GppcDatum to be converted and copied to ::GppcBpChar
 */
GppcBpChar		GppcDatumGetBpCharCopy(GppcDatum x);

/****************************************************************************
 * Text operations
 ****************************************************************************/
/**
 * \brief Returns byte size of this character string.
 * \param t ::GppcText holding character string.
 */
size_t GppcGetTextLength(GppcText t);

/**
 * \brief Returns the pointer to the head of character string.
 * \param t ::GppcText holding character string.
 *
 * Note the string is not null-terminated.  If you want a
 * null-terminated string, use GppcTextGetCString().  Also,
 * the memory pointed by this pointer may be the actual database
 * content.  Do not modify the memory content.
 */
char *GppcGetTextPointer(GppcText t);

/**
 * \brief Builds a null-terminated character string from ::GppcText.
 * \param t the original GppcText to build string from.
 *
 * The returned string is always copied locally and is therefore
 * mutable.
 */
char *GppcTextGetCString(GppcText t);

/**
 * \brief Builds ::GppcText from a null-terminated character string.
 * \param s character string to build ::GppcText from.
 *
 * This copies content from s and builds ::GppcText which can be returned
 * to the database.
 */
GppcText GppcCStringGetText(const char *s);

/**
 * \brief Returns byte size of this character string.
 * \param t ::GppcVarChar holding character string.
 */
size_t GppcGetVarCharLength(GppcVarChar t);

/**
 * \brief Returns the pointer to the head of character string.
 * \param t ::GppcVarChar holding character string.
 *
 * Note the string is not null-terminated.  If you want a
 * null-terminated string, use GppcVarCharGetCString().  Also,
 * the memory pointed by this pointer may be the actual database
 * content.  Do not modify the memory content.
 */
char *GppcGetVarCharPointer(GppcVarChar t);

/**
 * \brief Builds a null-terminated character string from ::GppcVarChar.
 * \param t the original GppcVarChar to build string from.
 *
 * The returned string is always copied locally hence it's mutable.
 */
char *GppcVarCharGetCString(GppcVarChar t);

/**
 * \brief Builds ::GppcVarChar from a null-terminated character string.
 * \param s character string to build ::GppcVarChar from.
 *
 * This copies content from s and builds ::GppcVarChar which can be returned
 * to the database.
 */
GppcVarChar GppcCStringGetVarChar(const char *s);

/**
 * \brief Returns byte size of this character string.
 * \param t ::GppcBpChar holding character string.
 */
size_t GppcGetBpCharLength(GppcBpChar t);

/**
 * \brief Returns the pointer to the head of character string.
 * \param t ::GppcBpChar holding character string.
 *
 * Note the string is not null-terminated.  If you want a
 * null-terminated string, use GppcBpCharGetCString().  Also,
 * the memory pointed by this pointer may be the actual database
 * content.  Do not modify the memory content.
 */
char *GppcGetBpCharPointer(GppcBpChar t);

/**
 * \brief Builds a null-terminated character string from ::GppcBpChar.
 * \param t the original GppcBpChar to build string from.
 *
 * The returned string is always copied locally hence it's mutable.
 */
char *GppcBpCharGetCString(GppcBpChar t);

/**
 * \brief Builds ::GppcBpChar from a null-terminated character string.
 * \param s character string to build ::GppcBpChar from.
 *
 * This copies content from s and builds ::GppcBpChar which can be returned
 * to the database.
 */
GppcBpChar GppcCStringGetBpChar(const char *s);

/****************************************************************************
 * Memory operations
 ****************************************************************************/
/**
 * \brief Allocates memory chunk in database context.
 * \param bytes byte size to be allocated.
 *
 * The allocated pointer can be freed by GppcFree() if it's not needed anymore,
 * but the database will free it after this context finishes.  The memory
 * content is uninitialized.  If the database cannot allocate memory,
 * the function will raise an error.  Thus, the caller doesn't need to check
 * NULL.
 */
void *GppcAlloc(size_t bytes);

/**
 * \brief Allocates a memory chunk in database context and initializes it.
 * \param bytes byte size to be allocated.
 *
 * The only difference from GppcAlloc() is the returned memory is initialized
 * to zero.
 */
void *GppcAlloc0(size_t bytes);

/**
 * \brief Resize the pre-allocated memory.
 * \param ptr pointer to the pre-allocated memory.
 * \param bytes byte size to be allocated.
 *
 * The returned pointer may be the same as input parameter, if the database
 * finds enough space after the pre-allocated memory.  Otherwise it allocates
 * another area and copy from the input.  If the database cannot allocate memory
 * this raises error.  Thus, the caller doesn't need to check NULL.
 */
void *GppcRealloc(void *ptr, size_t bytes);

/**
 * \brief Frees memory allocated by GppcAlloc(), GppcAlloc0() or GppcRealloc().
 * \param ptr pointer to pre-allocated memory.
 *
 * The memory allocated in database context may or may not be freed by
 * this function.  The database will free it later anyway.
 */
void GppcFree(void *ptr);

/**
 * \brief Creates a new ::GppcText instance.
 * \param clen character string length in bytes the instance will have.
 *
 * The returned ::GppcText is allocated in database context.
 */
GppcText GppcAllocText(size_t clen);

/**
 * \brief Creates a new ::GppcVarChar instance.
 * \param clen character string length in bytes the instance will have.
 *
 * The returned ::GppcVarChar is allocated in database context.
 */
GppcVarChar GppcAllocVarChar(size_t clen);

/**
 * \brief Creates a new ::GppcBpChar instance.
 * \param clen character string length in bytes the instance will have.
 *
 * The returned ::GppcBpChar is allocated in database context.
 */
GppcBpChar GppcAllocBpChar(size_t clen);


/****************************************************************************
 * Reports
 ****************************************************************************/
/**
 * \brief Report level used in GppcReport().
 * \sa Greenplum Administrator Guide
 */
typedef enum GppcReportLevel
{
	GPPC_DEBUG1				= 10,
	GPPC_DEBUG2				= 11,
	GPPC_DEBUG3				= 12,
	GPPC_DEBUG4				= 13,
	GPPC_DEBUG				= 14,
	GPPC_LOG				= 15,
	GPPC_INFO				= 17,
	GPPC_NOTICE				= 18,
	GPPC_WARNING			= 19,
	GPPC_ERROR				= 20,
} GppcReportLevel;

/**
 * \brief Emits a formatted message and may raise error.
 * \param elevel reporting level.
 * \param fmt format text.
 *
 * This function takes variadic arguments just as printf to format a string
 * and emit it to the frontend and/or log file, depending on the reporting
 * level and session configuration parameters such as client_min_messages.
 */
void GppcReport(GppcReportLevel elevel, const char *fmt, ...);


/****************************************************************************
 * Miscellaneous features
 ****************************************************************************/
/**
 * \brief Represents character encoding.
 */
typedef enum GppcEncoding
{
	GPPC_SQL_ASCII = 0,				/* SQL/ASCII */
	GPPC_EUC_JP = 1,				/* EUC for Japanese */
	GPPC_EUC_CN = 2,				/* EUC for Chinese */
	GPPC_EUC_KR = 3,				/* EUC for Korean */
	GPPC_EUC_TW = 4,				/* EUC for Taiwan */
	GPPC_EUC_JIS_2004 = 5,			/* EUC-JIS-2004 */
	GPPC_UTF8 = 6,					/* Unicode UTF8 */
	GPPC_MULE_INTERNAL = 7,			/* Mule internal code */
	GPPC_LATIN1 = 8,				/* ISO-8859-1 Latin 1 */
	GPPC_LATIN2 = 9,				/* ISO-8859-2 Latin 2 */
	GPPC_LATIN3 = 10,				/* ISO-8859-3 Latin 3 */
	GPPC_LATIN4 = 11,				/* ISO-8859-4 Latin 4 */
	GPPC_LATIN5 = 12,				/* ISO-8859-9 Latin 5 */
	GPPC_LATIN6 = 13,				/* ISO-8859-10 Latin6 */
	GPPC_LATIN7 = 14,				/* ISO-8859-13 Latin7 */
	GPPC_LATIN8 = 15,				/* ISO-8859-14 Latin8 */
	GPPC_LATIN9 = 16,				/* ISO-8859-15 Latin9 */
	GPPC_LATIN10 = 17,				/* ISO-8859-16 Latin10 */
	GPPC_WIN1256 = 18,				/* windows-1256 */
	GPPC_WIN1258 = 19,				/* Windows-1258 */
	GPPC_WIN866 = 20,				/* (MS-DOS CP866) */
	GPPC_WIN874 = 21,				/* windows-874 */
	GPPC_KOI8R = 22,					/* KOI8-R */
	GPPC_WIN1251 = 23,				/* windows-1251 */
	GPPC_WIN1252 = 24,				/* windows-1252 */
	GPPC_ISO_8859_5 = 25,			/* ISO-8859-5 */
	GPPC_ISO_8859_6 = 26,			/* ISO-8859-6 */
	GPPC_ISO_8859_7 = 27,			/* ISO-8859-7 */
	GPPC_ISO_8859_8 = 28,			/* ISO-8859-8 */
	GPPC_WIN1250 = 29,				/* windows-1250 */
	GPPC_WIN1253 = 30,				/* windows-1253 */
	GPPC_WIN1254 = 31,				/* windows-1254 */
	GPPC_WIN1255 = 32,				/* windows-1255 */
	GPPC_WIN1257 = 33,				/* windows-1257 */
	GPPC_KOI8U = 34					/* KOI8-U */
} GppcEncoding;

/**
 * \brief Returns the database encoding.
 */
GppcEncoding GppcGetDatabaseEncoding(void);

/**
 * \brief Returns maximum byte size of database encoding in a character.
 */
int GppcDatabaseEncodingMaxLength(void);

#ifdef BOOL_DEFINED
#undef bool
#endif

#endif   /* GPPC_H */
