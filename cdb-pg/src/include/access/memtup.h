/* 
 * In Memory Tuple format
 *
 * Copyright (c) 2008, Greenplum inc
 */
#ifndef MEM_TUP_H
#define MEM_TUP_H

#include "access/tupdesc.h"

/* 
 * TODO: implement this macro - equivalent
 * in functionality to the the following check
 * Assert(!(tup->t_data->t_infomask & HEAP_HASOID));
 * it's a sanity check.
 */
#define MemTupleNoOidSpace(tuple) Assert(tuple) \


typedef enum MemTupleBindFlag
{
	MTB_ByVal_Native = 1,	/* Fixed len, native (returned as datum ) */
	MTB_ByVal_Ptr    = 2,	/* Fixed len, convert to pointer for datum */
	MTB_ByRef  	 = 3,	/* var len */
	MTB_ByRef_CStr   = 4,   /* varlen, CString type */
} MemTupleBindFlag;

typedef struct MemTupleAttrBinding
{
	int offset; 		/* offset of attr in memtuple */
	int len;		/* lenth */
	MemTupleBindFlag flag;	/* binding flag */
	int null_byte;		/* which byte holds the null flag for the attr */
	unsigned char null_mask;		/* null bit mask */
} MemTupleAttrBinding;

typedef struct MemTupleBindingCols
{
	uint32 var_start; 	/* varlen fields start */
	MemTupleAttrBinding *bindings; /* bindings for attrs (cols) */
	short *null_saves;
} MemTupleBindingCols;

typedef struct MemTupleBinding
{
	TupleDesc tupdesc;
	int column_align;
	int null_bitmap_extra_size;  /* extra bytes required by null bitmap */

	MemTupleBindingCols bind;  	/* 2 bytes offsets */
	MemTupleBindingCols large_bind; /* large tup, 4 bytes offsets */
} MemTupleBinding;

typedef struct MemTupleData
{
	uint32 PRIVATE_mt_len;
	unsigned char PRIVATE_mt_bits[1]; 	/* varlen */
} MemTupleData;

typedef MemTupleData *MemTuple;

#define MEMTUP_LEAD_BIT 0x80000000
#define MEMTUP_LEN_MASK 0x7FFFFFF8
#define MEMTUP_HASNULL   1
#define MEMTUP_LARGETUP  2
#define MEMTUP_UNUSED 	 4
#define MEMTUP_ALIGN(LEN) TYPEALIGN(8, (LEN)) 
#define MEMTUPLE_LEN_FITSHORT 0xFFF0

static inline bool mtbind_has_oid(MemTupleBinding *pbind)
{
	return pbind->tupdesc->tdhasoid;
}

static inline bool is_len_memtuplen(uint32 len)
{
	return (len & MEMTUP_LEAD_BIT) != 0;
}
static inline bool memtuple_lead_bit_set(MemTuple tup)
{
	return (tup->PRIVATE_mt_len & MEMTUP_LEAD_BIT) != 0;
}
static inline uint32 memtuple_size_from_uint32(uint32 len)
{
	Assert ((len & MEMTUP_LEAD_BIT) != 0);
	return len & MEMTUP_LEN_MASK;
}
static inline uint32 memtuple_get_size(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	return (mtup->PRIVATE_mt_len & MEMTUP_LEN_MASK);
}
static inline void memtuple_set_size(MemTuple mtup, MemTupleBinding *pbind, uint32 len)
{
	UnusedArg(pbind);
	Assert((len & (~MEMTUP_LEN_MASK)) == 0); 
	mtup->PRIVATE_mt_len |= MEMTUP_LEAD_BIT;
	mtup->PRIVATE_mt_len = (mtup->PRIVATE_mt_len & (~MEMTUP_LEN_MASK)) | len;
}
static inline void memtuple_set_mtlen(MemTuple mtup, MemTupleBinding *pbind, uint32 mtlen)
{
	UnusedArg(pbind);
	Assert((mtlen & MEMTUP_LEAD_BIT) != 0);
	Assert((mtlen & MEMTUP_UNUSED) == 0);
	mtup->PRIVATE_mt_len = mtlen;
}
static inline bool memtuple_get_hasnull(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	return (mtup->PRIVATE_mt_len & MEMTUP_HASNULL) != 0;
}
static inline void memtuple_set_hasnull(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	mtup->PRIVATE_mt_len |= MEMTUP_HASNULL;
}
static inline void memtuple_clear_hasnull(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	mtup->PRIVATE_mt_len &= (~MEMTUP_HASNULL);
}
static inline bool memtuple_get_islarge(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	return (mtup->PRIVATE_mt_len & MEMTUP_LARGETUP) != 0;
}
static inline void memtuple_set_islarge(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	mtup->PRIVATE_mt_len |= MEMTUP_LARGETUP; 
}
static inline void memtuple_clear_islarge(MemTuple mtup, MemTupleBinding *pbind)
{
	UnusedArg(pbind);
	Assert(memtuple_lead_bit_set(mtup));
	Assert((mtup->PRIVATE_mt_len & MEMTUP_UNUSED) == 0);
	mtup->PRIVATE_mt_len &= (~MEMTUP_LARGETUP); 
}

extern void destroy_memtuple_binding(MemTupleBinding *pbind);
extern MemTupleBinding* create_memtuple_binding(TupleDesc tupdesc);

extern Datum memtuple_getattr(MemTuple mtup, MemTupleBinding *pbind, int attnum, bool *isnull);
extern bool memtuple_attisnull(MemTuple mtup, MemTupleBinding *pbind, int attnum);

extern MemTuple memtuple_copy_to(MemTuple mtup, MemTupleBinding *pbind, MemTuple dest, uint32 *destlen);
extern MemTuple memtuple_form_to(MemTupleBinding *pbind, Datum *values, bool *isnull, MemTuple dest, uint32 *destlen);

extern Oid MemTupleGetOid(MemTuple mtup, MemTupleBinding *pbind);
extern void MemTupleSetOid(MemTuple mtup, MemTupleBinding *pbind, Oid oid);

#endif /* _MEM_TUP_H_ */
