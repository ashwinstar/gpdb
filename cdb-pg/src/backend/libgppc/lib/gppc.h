/* ----------------------------------------------------------------------- *//**
 *
 * @file gppc.h
 *
 * @brief Greenplum Partner Connector main header
 *
 * This header consists of symbols that are exposed by libgppc.
 *
 * The SQL callable function must follow the GPPC call convension.  The first
 * rule is to declare function name by GPPC_FUNCTION_INFO_V1().  The second
 * is to have returned type as ::GppcDatum along with argument as
 * #GPPC_FUNCTION_ARGS.  The third is to use GPPC_GETARG_XXX() macros to
 * fetch the actual argument from the database and finally use
 * GPPC_RETURN_XXX() macro to return the result to SQL.
 *
 * \code
 *   GPPC_FUNCTION_INFO_V1(funcname);
 *   GppcDatum funcname(GPPC_FUNCTION_ARGS)
 *   {
 *     GppcXXX arg1 = GPPC_GETARG_XXXX(0);
 *     ....
 *     GPPC_RETURN_XXX(result);
 *   }
 * \endcode
 *
 * You can also use a utility macro GPPC_FUNCTION() to declare
 * #GPPC_FUNCTION_INFO_V1, function prototype, and function signature.
 *
 * Copyright (c) 2012, Greenplum Inc.
 *
 *//* ----------------------------------------------------------------------- */
#ifndef GPPC_H
#define GPPC_H

#include <sys/types.h>
#include <stdint.h>

#include "gppc_config.h"

/**
 * \brief GPPC_VERSION_NUM = MAJOR * 100 + MINOR
 */
#define GPPC_VERSION_NUM 100

/**
 * \brief Defines as the function is callable from SQL.
 * \param funcname the name of exposed function.
 *
 * You must define every function which you want to call
 * from SQL with this macro, so that the backend can call it
 * with the convension.
 */
#define GPPC_FUNCTION_INFO_V1(funcname) \
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

/**
 * \brief Useful macro to declare the exposed function.
 * \param funcname the name of exposed function.
 *
 * It actually declares GPPC_FUNCTION_INFO_V1, function prototype,
 * and function signature in one stop.  If you really need to declare
 * separate them like declaring prototype earlier, declare them
 * individually.
 */
#define GPPC_FUNCTION(funcname) \
GPPC_FUNCTION_INFO_V1(funcname); \
GppcDatum funcname (GPPC_FUNCTION_ARGS); \
GppcDatum \
funcname (GPPC_FUNCTION_ARGS)

#ifndef GPPC_C_BUILD
#ifndef bool
/**
 * \brief Generic boolean type if it's not defined.
 */
typedef char		bool;
#endif
#ifndef true
/**
 * \brief Generic boolean true if it's not defined.
 */
#define true 1
#endif
#ifndef false
/**
 * \brief Generic boolean false if it's not defined.
 */
#define false 0
#endif
#endif  /* GPPC_C_BUILD */

/****************************************************************************
 * GPPC types
 ****************************************************************************/
/**
 * \brief Opaque type which contains function call information.
 */
typedef struct GppcFcinfoData		   *GppcFcinfo;
/**
 * \brief Generic type which can be mapped to any other type.
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
 * \brief Two byte signed integer type which maps to SQL int2.
 */
typedef int16_t							GppcInt2;
/**
 * \brief Four byte signed integer type which maps to SQL int4.
 */
typedef int32_t							GppcInt4;
/**
 * \brief Eight byte signed integer type which maps to SQL int8.
 */
typedef int64_t							GppcInt8;
/**
 * \brief Four byte floating point type which maps to SQL float4.
 */
typedef float							GppcFloat4;
/**
 * \brief Eight byte floating point type which maps to SQL float8.
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
 * \brief Returns number of argument this function takes.
 */
#define GPPC_NARGS() (GppcNargs(fcinfo))
/**
 * \brief Finishes this function returning SQL NULL.
 */
#define GPPC_RETURN_NULL() return GppcReturnNull(fcinfo)
/**
 * \brief Call this to check if the argument is SQL NULL.
 * \param n the argument position starting from 0.
 */
#define GPPC_ARGISNULL(n) (GppcArgIsNull(fcinfo, n))

/****************************************************************************
 * GETARG macros
 ****************************************************************************/
/**
 * \brief Returns argument as ::GppcBool
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_BOOL(n)		(GppcDatumGetBool(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcChar
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_CHAR(n)		(GppcDatumGetChar(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcInt2
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT2(n)		(GppcDatumGetInt2(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcInt4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT4(n)		(GppcDatumGetInt4(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcInt4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_INT8(n)		(GppcDatumGetInt8(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcFloat4
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_FLOAT4(n)	(GppcDatumGetFloat4(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcFloat8
 * \param n the argument position starting from 0.
 */
#define GPPC_GETARG_FLOAT8(n)	(GppcDatumGetFloat8(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcText
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcText may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_TEXT(n)		(GppcDatumGetText(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcText
 * \param n the argument position starting from 0.
 *
 * In contrast to GPPC_GETARG_TEXT(), this returns a copy of the argument,
 * so the function can modify its content.
 */
#define GPPC_GETARG_TEXT_COPY(n)	(GppcDatumGetTextCopy(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcVarChar
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcVarChar may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_VARCHAR(n)	(GppcDatumGetVarChar(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcVarChar
 * \param n the argument position starting from 0.
 *
 * In contrast to GPPC_GETARG_VARCHAR(), this returns a copy of the argument,
 * so the function can modify its content.
 */
#define GPPC_GETARG_VARCHAR_COPY(n)	(GppcDatumGetVarCharCopy(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcBpChar
 * \param n the argument position starting from 0.
 *
 * The returned ::GppcBpChar may have a direct pointer to database content,
 * so do not modify the content.
 */
#define GPPC_GETARG_BPCHAR(n)	(GppcDatumGetBpChar(GppcGetArgDatum(fcinfo, n)))
/**
 * \brief Returns argument as ::GppcBpChar
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
 * \brief Finishes this function returning value as SQL bool.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_BOOL(x)		return GppcBoolGetDatum(x)
/**
 * \brief Finishes this function returning value as SQL "char".
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_CHAR(x)		return GppcCharGetDatum(x)
/**
 * \brief Finishes this function returning value as SQL int2.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT2(x)		return GppcInt2GetDatum(x)
/**
 * \brief Finishes this function returning value as SQL int4.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT4(x)		return GppcInt4GetDatum(x)
/**
 * \brief Finishes this function returning value as SQL int8.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_INT8(x)		return GppcInt8GetDatum(x)
/**
 * \brief Finishes this function returning value as SQL float4.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_FLOAT4(x)	return GppcFloat4GetDatum(x)
/**
 * \brief Finishes this function returning value as SQL float8.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_FLOAT8(x)	return GppcFloat8GetDatum(x)
/**
 * \brief Finishes this function returning value as SQL text.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_TEXT(x)		return GppcTextGetDatum(x)
/**
 * \brief Finishes this function returning value as SQL varchar.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_VARCHAR(x)	return GppcVarCharGetDatum(x)
/**
 * \brief Finishes this function returning value as SQL char.
 * \param x ::GppcDatum to be returned.
 */
#define GPPC_RETURN_BPCHAR(x)	return GppcBpCharGetDatum(x)

/**
 * \sa GPPC_FUNCTION_INFO_V1()
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
 * Note the string may not be null-terminated.  If you want a
 * null-terminated string, use GppcTextGetCString().  Also,
 * the memory pointed by this pointer may be the actual database
 * content.  Do not modify the memory content.
 */
char *GppcGetTextPointer(GppcText t);
/**
 * \brief Builds a null-terminated character string from ::GppcText.
 * \param t the original GppcText to build string from.
 *
 * The returned string is always copied locally hence it's mutable.
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
 * Note the string may not be null-terminated.  If you want a
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
 * Note the string may not be null-terminated.  If you want a
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
 * content is not initialized by zero.  If the database canoot allocate memory
 * this raises error.  Thus, the caller doesn't need to check NULL.
 */
void *GppcAlloc(size_t bytes);
/**
 * \brief Allocates memory chunk in database context and initializes it.
 * \param bytes byte size to be allocated.
 *
 * The only difference from GppcAlloc() is the returned memory is initialized
 * by zero.
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
 * \brief Frees memory allocated in database context.
 * \param ptr pointer to pre-allocated memory.
 *
 * The memory allocated in database context may or may not be freed by
 * this function.  The database will free it later anyway.
 */
void GppcFree(void *ptr);
/**
 * \brief Creates new ::GppcText instance.
 * \param clen character string length in bytes the instance will have.
 *
 * The returned ::GppcText is allocated in database context.
 */
GppcText GppcAllocText(size_t clen);
/**
 * \brief Creates new ::GppcVarChar instance.
 * \param clen character string length in bytes the instance will have.
 *
 * The returned ::GppcVarChar is allocated in database context.
 */
GppcVarChar GppcAllocVarChar(size_t clen);
/**
 * \brief Creates new ::GppcBpChar instance.
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
 * Miscs
 ****************************************************************************/
/**
 * \brief Returns true if the database encoding is UTF8(UNICODE).
 */
bool GppcIsDatabaseEncodingUtf8(void);
/**
 * \brief Returns maximum byte size of database encoding in a character.
 */
int GppcDatabaseEncodingMaxLength(void);

#endif   /* GPPC_H */
