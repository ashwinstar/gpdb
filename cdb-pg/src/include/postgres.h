/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  Primary include file for PostgreSQL server .c files
 *
 * This should be the first file included by PostgreSQL backend modules.
 * Client-side code should include postgres_fe.h instead.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/postgres.h,v 1.76 2007/01/05 22:19:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *----------------------------------------------------------------
 *	 TABLE OF CONTENTS
 *
 *		When adding stuff to this file, please try to put stuff
 *		into the relevant section, or add new sections as appropriate.
 *
 *	  section	description
 *	  -------	------------------------------------------------
 *		1)		variable-length datatypes (TOAST support)
 *		2)		datum type + support macros
 *		3)		exception handling definitions
 *		4)		genbki macros used by catalog/pg_xxx.h files
 *
 *	 NOTES
 *
 *	In general, this file should contain declarations that are widely needed
 *	in the backend environment, but are of no interest outside the backend.
 *
 *	Simple type definitions live in c.h, where they are shared with
 *	postgres_fe.h.	We do that since those type definitions are needed by
 *	frontend modules that want to deal with binary data transmission to or
 *	from the backend.  Type definitions in this file should be for
 *	representations that never escape the backend, such as Datum or
 *	TOASTed varlena objects.
 *
 *----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

#include "c.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "utils/elog.h"
#include "utils/palloc.h"

/* ----------------------------------------------------------------
 *				Section 1:	variable-length datatypes (TOAST support)
 * ----------------------------------------------------------------
 */

/* ----------------
 * struct varattrib is the header of a varlena object that may have been
 * TOASTed.  Generally, only the code closely associated with TOAST logic
 * should mess directly with struct varattrib or use the VARATT_FOO macros.
 * ----------------
 */

typedef union varattrib
{
	struct {
		uint8 	va_header;
		char 	va_data[1];
	} va_1byte;
	struct {
		uint8 	va_header;
		uint8	va_padding[3];
		int32	va_rawsize;		/* Plain data size */
		int32	va_extsize;		/* External saved size */
		Oid		va_valueid;		/* Unique identifier of value */
		Oid		va_toastrelid;	/* RelID where to find chunks */
	} va_external;
	struct 
	{
		uint32 	va_header;
		uint32	va_rawsize;		/* Plain data size */
		char	va_data[1];		/* Compressed data */
	} va_compressed;
	struct
	{
		uint32 	va_header;
		char 	va_data[1];
	} va_4byte;
} varattrib;

/* Externally visible macros (do these all belong here?) */


#define VARDATA(PTR)						VARDATA_4B((varattrib*)(PTR))
#define VARDATA_D(D)						VARDATA(DatumGetPointer(D))
#define VARSIZE(PTR) 						VARSIZE_4B((varattrib*)(PTR))
#define VARSIZE_D(D) 						VARSIZE(DatumGetPointer(D))

/* these are used by tuptoaster.c */
#define VARHDRSZ_SHORT 						1
#define VARSIZE_SHORT(PTR)					VARSIZE_1B((varattrib*)(PTR))
#define VARSIZE_SHORT_D(D)					VARSIZE_SHORT(DatumGetPointer(D))
#define VARDATA_SHORT(PTR)					VARDATA_1B((varattrib*)(PTR))
#define VARDATA_SHORT_D(D) 					VARDATA_SHORT(DatumGetPointer(D))
#define SET_VARSIZE(PTR, len)				SET_VARSIZE_4B  ((varattrib*)(PTR), (len))
#define SET_VARSIZE_SHORT(PTR, len) 		SET_VARSIZE_1B  ((varattrib*)(PTR), (len))
#define SET_VARSIZE_COMPRESSED(PTR, len) 	SET_VARSIZE_4B_C((varattrib*)(PTR), (len))
#define SET_VARSIZE_EXTERNAL(PTR) 			SET_VARSIZE_1B_E((varattrib*)(PTR))

/* Do we want to rename these? */
#define VARATT_IS_COMPRESSED(PTR) 			VARATT_IS_4B_C((varattrib*)(PTR))
#define VARATT_IS_COMPRESSED_D(D) 			VARATT_IS_COMPRESSED(DatumGetPointer(D))
#define VARATT_IS_EXTERNAL(PTR) 			VARATT_IS_1B_E((varattrib*)(PTR))
#define VARATT_IS_EXTERNAL_D(D) 			VARATT_IS_1B_E((varattrib*)( DatumGetPointer(D)))
#define VARATT_IS_SHORT(PTR) 				VARATT_IS_1B((varattrib*)(PTR))
#define VARATT_IS_SHORT_D(D) 				VARATT_IS_1B((varattrib*)( DatumGetPointer(D)))
#define VARATT_SET_COMPRESSED(PTR)			SET_VARSIZE_C((varattrib*)(PTR))
/* XXX */
#define VARATT_IS_EXTENDED(PTR)				(!VARATT_IS_4B_U((varattrib*)(PTR)))
#define VARATT_IS_EXTENDED_D(D)				VARATT_IS_EXTENDED(DatumGetPointer(D))

#define VARSIZE_EXTERNAL(PTR)				(sizeof(((varattrib*)(PTR))->va_external))
#define VARATT_EXTERNAL_IS_COMPRESSED(PTR)  (((varattrib*)(PTR))->va_external.va_extsize != \
											 ((varattrib*)(PTR))->va_external.va_rawsize - VARHDRSZ)
#define VARSIZE_ANY_EXHDR(PTR)				VARSIZE__ANY__EXHDR((varattrib*)(PTR))
#define VARSIZE_ANY_EXHDR_D(D)			    VARSIZE_ANY_EXHDR(DatumGetPointer(D))	
#define VARSIZE_ANY(PTR)					VARSIZE__ANY((varattrib*)(PTR))
#define VARSIZE_ANY_D(D)					VARSIZE_ANY(DatumGetPointer(D))
#define VARDATA_ANY(PTR)					VARDATA__ANY((varattrib*)(PTR))
#define VARDATA_ANY_D(D)					VARDATA_ANY(DatumGetPointer(D))

/* Internal macros used by heap_deform_tuple and tuptoaster.c and anywhere else
 * that wants to look at toasted tuples without detoasting them.
 */

/*
 * Bit patterns used to indicate sort varlena headers:
 *
 * 00xxxxxx	4-byte length word, aligned, uncompressed data (up to 1G)
 * 01xxxxxx	4-byte length word, aligned, *compressed* data (up to 1G)
 * 10000000	1-byte length word, unaligned, TOAST pointer
 * 1xxxxxxx	1-byte length word, unaligned, uncompressed data (up to 127b)
 *
 * Note: IS_1B is true for external toast records but VARSIZE_1B will return 0
 * for such records. As a consequence you must always check for for IS_EXTERNAL
 * before checking for IS_1B.
 */

#define VARATT_SHORT_MAX		0x7f
#define VARATT_COULD_SHORT(PTR)	(VARATT_IS_4B_U(PTR) && (VARSIZE(PTR)-VARHDRSZ+VARHDRSZ_SHORT <= VARATT_SHORT_MAX))
#define VARATT_COULD_SHORT_D(D)	VARATT_COULD_SHORT((varattrib *)(DatumGetPointer(D))) 
#define VARSIZE_TO_SHORT(PTR)	((char)(VARSIZE(PTR)-VARHDRSZ+VARHDRSZ_SHORT) | 0x80)
#define VARSIZE_TO_SHORT_D(D)	VARSIZE_TO_SHORT(DatumGetPointer(D))

#define VARDATA_4B(PTR) 		((PTR)->va_4byte.va_data)
#define VARDATA_4B_C(PTR) 		((PTR)->va_compressed.va_data)
#define VARDATA_1B(PTR) 		((PTR)->va_1byte.va_data)

#define VARATT_IS_4B(PTR)		(((PTR)->va_1byte.va_header & 0x80) == 0x00)
#define VARATT_IS_4B_U(PTR)		(((PTR)->va_1byte.va_header & 0xC0) == 0x00)
#define VARATT_IS_4B_C(PTR)		(((PTR)->va_1byte.va_header & 0xC0) == 0x40)
#define VARATT_IS_1B(PTR)		(((PTR)->va_1byte.va_header & 0x80) == 0x80)
#define VARATT_IS_1B_E(PTR)		(((PTR)->va_1byte.va_header       ) == 0x80)

/* VARSIZE_4B() should only be used on aligned data */
#define VARSIZE_4B(PTR) 		(ntohl((PTR)->va_4byte.va_header) & 0x3FFFFFFF)
#define VARSIZE_1B(PTR) 		(     ((PTR)->va_1byte.va_header) & 0x7F)

#define SET_VARSIZE_4B(PTR,len) 	((PTR)->va_4byte.va_header = htonl( (len) & 0x3fffffff ))
#define SET_VARSIZE_4B_C(PTR,len) 	((PTR)->va_4byte.va_header = htonl( (len) & 0x3fffffff | 0x40000000))
#define SET_VARSIZE_1B(PTR,len) 	((PTR)->va_1byte.va_header =        (len) | 0x80)
#define SET_VARSIZE_1B_E(PTR)		((PTR)->va_1byte.va_header = 0x80)
#define SET_VARSIZE_C(PTR)			((PTR)->va_1byte.va_header |= 0x40)

/* this is a bit weird if you pass in an external toast pointer, VARDATA
 * returns the original datum and VARSIZE_EXHDR returns the size of the whole
 * pointer including the header. That might be wrong but it works for the
 * assertion VARSIZE_EXHDR is being used for.
 */

#define VARSIZE__ANY__EXHDR(PTR)									\
	(VARATT_IS_1B_E(PTR) ? sizeof((PTR)->va_external) :				\
	 (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR)-1 :						\
	  VARSIZE_4B(PTR)-4 ))

#define VARSIZE__ANY(PTR)											\
	(VARATT_IS_1B_E(PTR) ? sizeof((PTR)->va_external) :				\
	 (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR) :							\
	  VARSIZE_4B(PTR) ))

#define VARDATA__ANY(PTR)											\
	(VARATT_IS_1B_E(PTR) ? (char*)(PTR) :							\
	 (VARATT_IS_1B(PTR) ? VARDATA_1B(PTR) :							\
	  VARDATA_4B(PTR) ))

#define VARDATA_COMPRESSED(PTR)	((PTR)->va_compressed.va_data)


/* ----------------------------------------------------------------
 *				Section 2:	datum type + support macros
 * ----------------------------------------------------------------
 */

/*
 * Port Notes:
 *
 *  Greenplum CDB:
 * 	Datum is alway 8 bytes, regardless if it is 32bit or 64bit machine.  
 *
 *	Postgres also assumes that
 *
 *	sizeof(char) == 1
 *
 *	and that
 *
 *	sizeof(short) == 2
 *
 *	If your machine meets these requirements, Datums should also be checked
 *	to see if the positioning is correct.
 */

typedef int64 Datum;
typedef union Datum_U
{
	Datum d;

	float4 f4;
	float8 f8;

	void *ptr;
} Datum_U;

#define SIZEOF_DATUM (sizeof(Datum))
typedef Datum *DatumPtr;

/* 
 * Conversion between Datum and type X.  Changed from Macro to static inline
 * functions to get proper type checking.
 */

static inline bool DatumGetBool(Datum d) { return d != 0; }
static inline Datum BoolGetDatum(bool b) { return (b ? 1 : 0); } 

static inline char DatumGetChar(Datum d) { return (char) d; } 
static inline Datum CharGetDatum(char c) { return (Datum) c; } 

static inline int8 DatumGetInt8(Datum d) { return (int8) d; } 
static inline Datum Int8GetDatum(int8 i8) { return (Datum) i8; }

static inline uint8 DatumGetUInt8(Datum d) { return (uint8) d; } 
static inline Datum UInt8GetDatum(uint8 ui8) { return (Datum) ui8; } 

static inline int16 DatumGetInt16(Datum d) { return (int16) d; } 
static inline Datum Int16GetDatum(int16 i16) { return (Datum) i16; } 

static inline uint16 DatumGetUInt16(Datum d) { return (uint16) d; } 
static inline Datum UInt16GetDatum(uint16 ui16) { return (Datum) ui16; } 

static inline int32 DatumGetInt32(Datum d) { return (int32) d; } 
static inline Datum Int32GetDatum(int32 i32) { return (Datum) i32; } 

static inline uint32 DatumGetUInt32(Datum d) { return (uint32) d; } 
static inline Datum UInt32GetDatum(uint32 ui32) { return (Datum) ui32; } 

static inline int64 DatumGetInt64(Datum d) { return (int64) d; } 
static inline Datum Int64GetDatum(int64 i64) { return (Datum) i64; } 
static inline Datum Int64GetDatumFast(int64 x) { return Int64GetDatum(x); } 

static inline uint64 DatumGetUInt64(Datum d) { return (uint64) d; } 
static inline Datum UInt64GetDatum(uint64 ui64) { return (Datum) ui64; } 

static inline Oid DatumGetObjectId(Datum d) { return (Oid) d; } 
static inline Datum ObjectIdGetDatum(Oid oid) { return (Datum) oid; } 

static inline TransactionId DatumGetTransactionId(Datum d) { return (TransactionId) d; } 
static inline Datum TransactionIdGetDatum(TransactionId tid) { return (Datum) tid; } 

static inline CommandId DatumGetCommandId(Datum d) { return (CommandId) d; } 
static inline Datum CommandIdGetDatum(CommandId cid) { return (Datum) cid; } 

static inline void *DatumGetPointer(Datum d) { Datum_U du; du.d = d; return du.ptr; }
static inline Datum PointerGetDatum(void *p) { Datum_U du; du.d = 0; du.ptr = p; return du.d; } 

static inline char *DatumGetCString(Datum d) { return (char* ) DatumGetPointer(d); } 
static inline Datum CStringGetDatum(char *p) { return PointerGetDatum(p); }

static inline Name DatumGetName(Datum d) { return (Name) DatumGetPointer(d); }
static inline Datum NameGetDatum(Name n) { return PointerGetDatum(n); } 

static inline float4 DatumGetFloat4(Datum d) { Datum_U du; du.d = d; return du.f4; } 
static inline Datum Float4GetDatum(float4 f) { Datum_U du; du.d = 0; du.f4 = f; return du.d; } 

static inline float8 DatumGetFloat8(Datum d) { Datum_U du; du.d = d; return du.f8; } 
static inline Datum Float8GetDatum(float8 f) { Datum_U du; du.f8 = f; return du.d; }
static inline Datum Float8GetDatumFast(float8 f) { return Float8GetDatum(f); }

/* ----------------------------------------------------------------
 *				Section 3:	exception handling definitions
 *							Assert, Trap, etc macros
 * ----------------------------------------------------------------
 */

extern DLLIMPORT bool assert_enabled;

/*
 * USE_ASSERT_CHECKING, if defined, turns on all the assertions.
 * - plai  9/5/90
 *
 * It should _NOT_ be defined in releases or in benchmark copies
 */

/*
 * Trap
 *		Generates an exception if the given condition is true.
 */
#define Trap(condition, errorType) \
	do { \
		if ((assert_enabled) && (condition)) \
			ExceptionalCondition(CppAsString(condition), (errorType), \
								 __FILE__, __LINE__); \
	} while (0)

/*
 *	TrapMacro is the same as Trap but it's intended for use in macros:
 *
 *		#define foo(x) (AssertMacro(x != 0) && bar(x))
 *
 *	Isn't CPP fun?
 */
#define TrapMacro(condition, errorType) \
	((bool) ((! assert_enabled) || ! (condition) || \
			 (ExceptionalCondition(CppAsString(condition), (errorType), \
								   __FILE__, __LINE__))))

#ifndef USE_ASSERT_CHECKING
#define Assert(condition)
#define AssertMacro(condition)	((void)true)
#define AssertArg(condition)
#define AssertState(condition)
#define AssertImply(condition1, condition2)
#define AssertEquivalent(cond1, cond2)
#else
#define Assert(condition) \
		Trap(!(condition), "FailedAssertion")

#define AssertMacro(condition) \
		((void) TrapMacro(!(condition), "FailedAssertion"))

#define AssertArg(condition) \
		Trap(!(condition), "BadArgument")

#define AssertState(condition) \
		Trap(!(condition), "BadState")

#define AssertImply(cond1, cond2) \
		Trap(!(!(cond1) || (cond2)), "AssertImply failed")

#define AssertEquivalent(cond1, cond2) \
		Trap(!((bool)(cond1) == (bool)(cond2)), "AssertEquivalent failed")

#endif   /* USE_ASSERT_CHECKING */

extern int ExceptionalCondition(const char *conditionName,
                                const char *errorType,
                                const char *fileName,
                                int lineNumber);
extern int SyncAgentMain(int, char **, const char *);

/* ----------------------------------------------------------------
 *				Section 4: genbki macros used by catalog/pg_xxx.h files
 * ----------------------------------------------------------------
 */
#define CATALOG(name,oid)	typedef struct CppConcat(FormData_,name)

#define BKI_BOOTSTRAP
#define BKI_SHARED_RELATION
#define BKI_WITHOUT_OIDS

/* these need to expand into some harmless, repeatable declaration */
#define DATA(x)   extern int no_such_variable
#define DESCR(x)  extern int no_such_variable
#define SHDESCR(x) extern int no_such_variable

typedef int4 aclitem;			/* PHONY definition for catalog use only */

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif   /* POSTGRES_H */
