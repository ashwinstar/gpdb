/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  Tuple macros used by both index tuples and heap tuples.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/tupmacs.h,v 1.31 2007/01/05 22:19:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_magic_oid.h"
#include "catalog/pg_type.h"

/*
 * check to see if the ATT'th bit of an array of 8-bit bytes is set.
 */
#define att_isnull(ATT, BITS) (!((BITS)[(ATT) >> 3] & (1 << ((ATT) & 0x07))))

/*
 * Given a Form_pg_attribute and a pointer into a tuple's data area,
 * return the correct value or pointer.
 *
 * We return a Datum value in all cases.  If the attribute has "byval" false,
 * we return the same pointer into the tuple data area that we're passed.
 * Otherwise, we return the correct number of bytes fetched from the data
 * area and extended to Datum form.
 *
 * On machines where Datum is 8 bytes, we support fetching 8-byte byval
 * attributes; otherwise, only 1, 2, and 4-byte values are supported.
 *
 * Note that T must already be properly aligned for this to work correctly.
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * Same, but work from byval/len parameters rather than Form_pg_attribute.
 */
#define fetch_att(T,attbyval,attlen) \
( \
	(attbyval) ? \
	( \
		(attlen) == (int) sizeof(Datum) ? \
			*((Datum *)(T)) \
		: \
	  ( \
		(attlen) == (int) sizeof(int32) ? \
			Int32GetDatum(*((int32 *)(T))) \
		: \
		( \
			(attlen) == (int) sizeof(int16) ? \
				Int16GetDatum(*((int16 *)(T))) \
			: \
			( \
				AssertMacro((attlen) == 1), \
				CharGetDatum(*((char *)(T))) \
			) \
		) \
	  ) \
	) \
	: \
	PointerGetDatum((char *) (T)) \
)

/*
 * att_align aligns the given offset as needed for a datum of alignment
 * requirement attalign.  The cases are tested in what is hopefully something
 * like their frequency of occurrence.
 */
#define att_align(cur_offset, attalign) \
( \
	((attalign) == 'i') ? INTALIGN(cur_offset) : \
	 (((attalign) == 'c') ? ((long)(cur_offset)) : \
	  (((attalign) == 'd') ? DOUBLEALIGN(cur_offset) : \
		( \
			AssertMacro((attalign) == 's'), \
			SHORTALIGN(cur_offset) \
		))) \
)

/*
 * att_align_nominal aligns the given offset as needed for a datum of alignment
 * requirement attalign, ignoring any consideration of packed varlena datums.
 * There are three main use cases for using this macro directly:
 *	* we know that the att in question is not varlena (attlen != -1);
 *	  in this case it is cheaper than the above macros and just as good.
 *	* we need to estimate alignment padding cost abstractly, ie without
 *	  reference to a real tuple.  We must assume the worst case that
 *	  all varlenas are aligned.
 *	* within arrays, we unconditionally align varlenas (XXX this should be
 *	  revisited, probably).
 *
 * The attalign cases are tested in what is hopefully something like their
 * frequency of occurrence.
 */
#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == 'i') ? INTALIGN(cur_offset) : \
	 (((attalign) == 'c') ? (long) (cur_offset) : \
	  (((attalign) == 'd') ? DOUBLEALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == 's'), \
			SHORTALIGN(cur_offset) \
	   ))) \
)

/*
 * att_addlength_datum increments the given offset by the space needed for
 * the given Datum variable.  attdatum is only accessed if we are dealing
 * with a variable-length attribute.
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer performs the same calculation as att_addlength_datum,
 * but is used when walking a tuple --- attptr is the pointer to the field
 * within the tuple.
 *
 * Note: some callers pass a "char *" pointer for cur_offset.  This is
 * actually perfectly OK, but probably should be cleaned up along with
 * the same practice for att_align_pointer.
 */
 #define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((char *) (attptr)) + 1) \
	)) \
)

#define att_addlength(cur_offset, attlen, attval) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(DatumGetPointer(attval)) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen(DatumGetCString(attval)) + 1) \
	)) \
)

/*
 * store_att_byval is a partial inverse of fetch_att: store a given Datum
 * value into a tuple data area at the specified address.  However, it only
 * handles the byval case, because in typical usage the caller needs to
 * distinguish by-val and by-ref cases anyway, and so a do-it-all macro
 * wouldn't be convenient.
 */
#define store_att_byval(T,newdatum,attlen) \
	do { \
		switch (attlen) \
		{ \
			case sizeof(char): \
				*(char *) (T) = DatumGetChar(newdatum); \
				break; \
			case sizeof(int16): \
				*(int16 *) (T) = DatumGetInt16(newdatum); \
				break; \
			case sizeof(int32): \
				*(int32 *) (T) = DatumGetInt32(newdatum); \
				break; \
			case sizeof(Datum): \
				*(Datum *) (T) = (newdatum); \
				break; \
			default: \
				elog(ERROR, "unsupported byval length: %d", \
					 (int) (attlen)); \
				break; \
		} \
	} while (0)

/* Proper align with zero padding */
static inline char * att_align_zero(char *data, char alignchar)
{
    size_t  misalignment = (size_t)att_align(1, alignchar) - 1;

    while ((size_t)data & misalignment)
		*(data++) = 0;

	return data;
}

/* Determin if a datum of type oid can be stored in short varlena format */
static inline bool value_type_could_short(Datum d, Oid typid)
{
        return	!VARATT_IS_EXTERNAL_D(d) &&
                (VARATT_IS_SHORT_D(d) || 
                 (VARATT_COULD_SHORT_D(d) &&
                  typid != INT2VECTOROID &&
                  typid != OIDVECTOROID &&
                  typid < FirstNormalObjectId));
}
#endif
