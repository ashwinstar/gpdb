/* 
 * MemTuple
 * 
 * Copyright (c) 2006-2008, Greenplum inc
 */

#include "postgres.h"

#include "access/memtup.h"
#include "access/tupmacs.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "catalog/pg_type.h" 

/* Memory tuple forat:
 * 4 byte _mt_len,
 * 	highest bit always 1.  (if 0, means it is a heaptuple).
 * 	bit 2-29th is memtuple length, in bytes.  It is always 8
 * 	bytes aligned.
 * 	bit 30, 31 is unused.
 *	bit 32 is if has null.
 *
 * 4 byte null bitmaps.  If no null, this is 4 bytes padding.
 * if has null, and more than 32 attributes, then the mull bitmap
 * continues null_bitmap_extra_size byte.  The end of null bitmap
 * is 8 bytes aligned.
 *
 * Non-Null attribute:
 * 	Fixed len Attributes.
 *		The attributes are not in logical order.  First we put 8 bytes 
 * 		aligned native or native_ptr types.  The 4 bytes aligned natives
 * 		then 2 bytes aligned and varlena, then 1 bytes aligned natives.
 *		Varlena occupy 2bytes in the fixed len area.
 *
 * 	Varlena attributes.
 *
 * The end is again padded to 8 bytes aligned.
 *
 * Null attributes only occupy one bit in the nullbit map.  The non null
 * attributes is located from the binding offset/len.  If there is null attr,
 * we use the null_saves in the binding to figure out how many columns that is
 * physically precedes the attribute is null and how much space we have saved,
 * then we use off minus saved bytes to find the attribute.
 */

#define MEMTUP_DFLT_NULL_BITMAP_BYTES 4

static inline int compute_null_bitmap_extra_size(int natts)
{
	int nbytes = (natts + 7) >> 3;
	if (nbytes <= MEMTUP_DFLT_NULL_BITMAP_BYTES)
		return 0;
	return MEMTUP_ALIGN((nbytes - MEMTUP_DFLT_NULL_BITMAP_BYTES));
}

void destroy_memtuple_binding(MemTupleBinding *pbind)
{
	Assert(pbind && pbind->bindings);
	if(pbind->null_saves)
		pfree(pbind->null_saves);
	pfree(pbind->bindings);
	pfree(pbind);
}

/*
 * Manage the space saved by not storing nulls.  
 * Attr are rearranged in the order of 8 bytes aligned, then 4,
 * then 2, then 1.  A bit in the null bitmap is set for each 
 * null attribute.  For all possible combinations of 4 null bit,
 * we index into a short[16] array to get how many space is saved
 * by the nulls.
 */

/* Compute how much space to store the null save entries.
 * The null save entries are stored in the binding, not per tuple.
 */
static inline uint32 compute_null_save_entries(int i)
{
	return ((i+7)/8) * 32; 
}

/* Add null save space into the entries */
static inline void add_null_save(short *null_save, int i, short sz)
{
	short* first = null_save + ((i/4) * 16);
	unsigned int bit = 1 << (i%4);

	for(i=0; i<16; ++i)
	{
		if( (i & bit) != 0)
			first[i] += sz;
	}
}

/* Compute how much bytes are saved by one byte in the null bit map */
static inline short compute_null_save_b(short *null_saves, unsigned char b)
{
	unsigned int blow = (b & 0xF);
	unsigned int bhigh = (b >> 4);

	return null_saves[blow] + null_saves[16+bhigh];
}

/* compute the null saved bytes by the whole null bit map, by the attribute
 * physically precedes the one.
 */
static inline short compute_null_save(short *null_saves, unsigned char *nullbitmaps, int nbyte, unsigned char nbit)
{
	short ret = 0;
	int curr_byte = 0;
	while(curr_byte < nbyte) 
	{
		ret += compute_null_save_b(null_saves, nullbitmaps[curr_byte]);
		null_saves += 32;
		++curr_byte;
	}

	ret += compute_null_save_b(null_saves, (nullbitmaps[nbyte] & (nbit-1)));
	return ret;
}
		
/* Whether a type could short.
 * Oringianlly postgres if very aggressive, it saves all system types
 * except int2vector and oidvector.  This is potentially a problem because
 * if the varlena needs an alignment, we need to palloc and memcpy.  It is 
 * very hard to determine if the space saved on disk is worth the CPU cost.
 * Leave it as is for now.
 */
static inline bool typid_could_short(int typid)
{
#if 1
	return ( typid != INT2VECTOROID && 
			typid != OIDVECTOROID &&
			typid < FirstNormalObjectId
	       );
#else
	return (typid == BYTEAOID || typid == TEXTOID 
			|| typid == BPCHAROID || typid == VARCHAROID 
			|| typid == NUMERICOID || typid == CSTRINGOID
	       );
#endif
}

/* Create a memtuple binding from the tupdesc.  Note we store
 * a ref to the tupdesc in the binding, so we assumed the life
 * span of the tupdesc is no shorter than the binding.
 */
MemTupleBinding *create_memtuple_binding(TupleDesc tupdesc) 
{
	int i = 0;
	int physical_col = 0;
	int pass = 0;

	uint32 null_save_entries = 0;
	uint32 cur_offset = 8;

	MemTupleBinding *pbind = (MemTupleBinding *) palloc(sizeof(MemTupleBinding));

	pbind->tupdesc = tupdesc;
	pbind->null_bitmap_extra_size = compute_null_bitmap_extra_size(tupdesc->natts);

	/* alloc null save entries.  Zero it */
	null_save_entries = compute_null_save_entries(tupdesc->natts);
	pbind->null_saves = (short *) palloc0(sizeof(short) * null_save_entries);

	/* alloc bindings, no need to zero because we will fill them out  */
	pbind->bindings = (MemTupleAttrBinding *) palloc(sizeof(MemTupleAttrBinding) * tupdesc->natts);
	
	/*
	 * First pass, do 8 bytes aligned, native type.
	 * Sencond pass, do 4 bytes aligned, native type.
	 * Third pass, do 2 bytes aligned, native type and var len type.
	 * Finall, do 1 bytes aligned native type.
	 */
	for(pass =0; pass < 4; ++pass)
	{
		for(i=0; i<tupdesc->natts; ++i)
		{
			Form_pg_attribute attr = tupdesc->attrs[i];
			MemTupleAttrBinding *bind = &pbind->bindings[i];

			if(pass == 0 && attr->attlen > 0 && attr->attalign == 'd')
			{
				bind->offset = att_align(cur_offset, attr->attalign);
				bind->len = attr->attlen;
				add_null_save(pbind->null_saves, physical_col, attr->attlen);

				bind->flag = attr->attbyval ? MTB_ByVal_Native : MTB_ByVal_Ptr;
				bind->null_byte = physical_col >> 3;
				bind->null_mask = 1 << (physical_col-(bind->null_byte << 3));

				physical_col += 1;
				cur_offset = bind->offset + bind->len;
			}
			else if (pass == 1 && attr->attlen > 0 && attr->attalign == 'i')
			{
				bind->offset = att_align(cur_offset, attr->attalign);
				bind->len = attr->attlen;
				add_null_save(pbind->null_saves, physical_col, attr->attlen);

				bind->flag = attr->attbyval ? MTB_ByVal_Native : MTB_ByVal_Ptr;
				bind->null_byte = physical_col >> 3;
				bind->null_mask = 1 << (physical_col-(bind->null_byte << 3));

				physical_col += 1;
				cur_offset = bind->offset + bind->len;
			}
			else if (pass == 2 && ( (attr->attlen > 0 && attr->attalign == 's') 
						|| ( attr->attlen < 0
						     && (
							     attr->atttypid != BPCHAROID 
							     || attr->atttypmod <= 4
							     || attr->atttypmod >= 127
							)
						     )
					      )
				)
			{
				bind->offset = att_align(cur_offset, 's');
				bind->len = attr->attlen > 0 ? attr->attlen : sizeof(short);
				add_null_save(pbind->null_saves, physical_col, bind->len);
				
				if(attr->attlen > 0)
					bind->flag = attr->attbyval ? MTB_ByVal_Native : MTB_ByVal_Ptr;
				else if(attr->attlen == -1)
					bind->flag = MTB_ByRef;
				else
				{
					Assert(attr->attlen == -2);
					bind->flag = MTB_ByRef_CStr;
				}

				bind->null_byte = physical_col >> 3;
				bind->null_mask = 1 << (physical_col-(bind->null_byte << 3));

				physical_col += 1;
				cur_offset = bind->offset + bind->len;
			}
			else if (pass == 3 && (
						(attr->attlen > 0 && attr->attalign == 'c') 
						|| (attr->attlen < 0 
							&& attr->atttypid == BPCHAROID 
							&& attr->atttypmod > 4 
							&& attr->atttypmod < 127
						   )
					      )
				)
			{
				bind->offset = att_align(cur_offset, 'c');
				bind->len = attr->attlen > 0 ? attr->attlen : (attr->atttypmod - 3);

				add_null_save(pbind->null_saves, physical_col, 1);

				if(attr->attlen > 0 && attr->attbyval)
					bind->flag = MTB_ByVal_Native;
				else
					bind->flag = MTB_ByVal_Ptr;

				bind->null_byte = physical_col >> 3;
				bind->null_mask = 1 << (physical_col-(bind->null_byte << 3));

				physical_col += 1;
				cur_offset = bind->offset + bind->len;
			}
		}
	}

	if(tupdesc->natts != 0)
		pbind->var_start = cur_offset;
	else
		pbind->var_start = 8;

	Assert(pbind->var_start >= 8);
	Assert(pbind->tupdesc->natts == physical_col);

	return pbind;
}
			
/* Compute the memtuple size. 
 * nullsave is an output param
 */
static uint32 compute_memtuple_size(MemTupleBinding *pbind, Datum *values, bool *isnull, bool hasnull, uint32 *nullsaves) 
{
	uint32 data_length = pbind->var_start;
	int i;

	*nullsaves = 0;

	if(hasnull)
		data_length += pbind->null_bitmap_extra_size;

	for(i=0; i<pbind->tupdesc->natts; ++i)
	{
		if(isnull[i])
		{
			MemTupleAttrBinding *bind = &pbind->bindings[i];
			*nullsaves += bind->len;
			data_length -= bind->len;
		}
	}

	for(i=0; i<pbind->tupdesc->natts; ++i)
	{
		MemTupleAttrBinding *bind = &pbind->bindings[i];
		Form_pg_attribute attr = pbind->tupdesc->attrs[i];

		if(isnull[i] || bind->flag == MTB_ByVal_Native || bind->flag == MTB_ByVal_Ptr)
			continue;

		/* Varlen stuff */
		/* We plan to convert to short varlena even if it is not currently */
		if(bind->flag == MTB_ByRef 
				&& !VARATT_IS_EXTERNAL_D(values[i])
				&& (VARATT_IS_SHORT_D(values[i]) 
					|| (VARATT_COULD_SHORT_D(values[i])
						&& typid_could_short(attr->atttypid)
					   )
				   )
		  ) 
		{
			data_length += VARSIZE_ANY_EXHDR_D(values[i]) + VARHDRSZ_SHORT;
		}
		else
		{
			data_length = att_align(data_length, attr->attalign); 
			data_length = att_addlength(data_length, attr->attlen, values[i]);
		}
	}

	return MEMTUP_ALIGN(data_length);
}

static inline char* memtuple_get_attr_ptr(char *start, MemTupleAttrBinding *bind, short *null_saves, unsigned char *nullp)
{
	short ns = 0;

	Assert(bind->offset >= 8);
	if(nullp)
		ns = compute_null_save(null_saves, nullp, bind->null_byte, bind->null_mask);
	return start + bind->offset - ns;
}

static inline char* memtuple_get_attr_data_ptr(char *start, MemTupleAttrBinding *bind, short *null_saves, unsigned char* nullp)
{
	Assert(bind->offset >= 8);
	switch(bind->flag)
	{
		case MTB_ByVal_Native:
		case MTB_ByVal_Ptr:
			return memtuple_get_attr_ptr(start, bind, null_saves, nullp);
		default:
			return start + (*(short *) memtuple_get_attr_ptr(start, bind, null_saves, nullp));
	}
}

/* form a memtuple from values and isnull, to a prespecified buffer */
MemTuple memtuple_form_to(MemTupleBinding *pbind,
		Datum *values, bool *isnull,
		MemTuple mtup, uint32 *destlen
		)
{
	bool hasnull = false;
	int i;
	uint32 len;
	unsigned char *nullp;
	char *start;
	char *varlen_start;
	uint32 null_save_len;

	/*
	 * Check for nulls and embedded tuples; expand any toasted attributes in
	 * embedded tuples.  This preserves the invariant that toasting can only
	 * go one level deep.
	 *
	 * We can skip calling toast_flatten_tuple_attribute() if the attribute
	 * couldn't possibly be of composite type.  All composite datums are
	 * varlena and have alignment 'd'; furthermore they aren't arrays. Also,
	 * if an attribute is already toasted, it must have been sent to disk
	 * already and so cannot contain toasted attributes.
	 */
	for(i=0; i<pbind->tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = pbind->tupdesc->attrs[i];
		
#ifdef CHK_TYPE_SANE
		check_type_sanity(attr, values[i], isnull[i]);
#endif

		if(isnull[i])
		{
			hasnull = true;
			continue;
		}

		if (attr->attlen == -1 &&
				attr->attalign == 'd' &&
				attr->attndims == 0 &&
				!VARATT_IS_EXTENDED_D(values[i]))
		{
			/*
			 * FIXME:
			 * XXX The following is a bug -- unless the caller calls 
			 * reset, we will leak.  
			 */
#if 0
			Assert(!"Who owns the memory?  It will leak!");
#endif
			values[i] = toast_flatten_tuple_attribute(values[i], attr->atttypid, attr->atttypmod);
		}

		if (attr->attlen == -1 && VARATT_IS_EXTERNAL_D(values[i]))
			values[i] = PointerGetDatum(heap_tuple_fetch_attr(DatumGetPointer(values[i])));
	}

	/* compute needed length */
	len = compute_memtuple_size(pbind, values, isnull, hasnull, &null_save_len);

	if(!destlen)
	{
		Assert(!mtup);
		mtup = (MemTuple) palloc(len);
	}
	else if(*destlen < len)
	{
		*destlen = len;
		return NULL;
	}
	else
	{
		*destlen = len;
		Assert(mtup);
	}

	/* Set mtlen, this set the lead bit, len, and clears hasnull bit 
	 * because the len returned from compute size is always max aligned
	 */
	Assert(len == MEMTUP_ALIGN(len));
	memtuple_set_mtlen(mtup, pbind, (len | MEMTUP_LEAD_BIT)); 

	nullp = (hasnull ? mtup->PRIVATE_mt_bits : NULL);
	start = (char *) mtup;
	varlen_start = ((char *) mtup) + pbind->var_start - null_save_len;

	if(hasnull)
	{
		memtuple_set_hasnull(mtup, pbind);

		/* if null bitmap is more than 4 bytes, add needed space */
		start += pbind->null_bitmap_extra_size;
		varlen_start += pbind->null_bitmap_extra_size;

		/* clear null bitmap */
		memset(nullp, 0, 4 + pbind->null_bitmap_extra_size);
	}

	/* It is very important to setup the null bitmap first before we 
	 * really put the values into place.  Where is the value in the 
	 * memtuple is determined by space saved from nulls, so the bitmap
	 * is used in the next loop. 
	 * NOTE: We cannot set the bitmap in the next loop (even at very
	 * beginning of next loop), because physical col order is different
	 * from logical. 
	 */
	for(i=0; i<pbind->tupdesc->natts; ++i)
	{
		if(isnull[i])
		{
			MemTupleAttrBinding *bind = &(pbind->bindings[i]);
			Assert(hasnull);
			nullp[bind->null_byte] |= bind->null_mask;
		}
	}

	/* Null bitmap is set up correctly, we can put in values now */
	for(i=0; i<pbind->tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = pbind->tupdesc->attrs[i];
		MemTupleAttrBinding *bind = &(pbind->bindings[i]);

		uint32 attr_len;

		if(isnull[i])
			continue;

		Assert(bind->offset != 0);

		/* Not null */
		switch(bind->flag)
		{
			case MTB_ByVal_Native:
				store_att_byval(memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp),
						values[i],
						bind->len
					       );
				break;
			case MTB_ByVal_Ptr:
				if(attr->atttypid != BPCHAROID)
					memcpy(memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp), 
							DatumGetPointer(values[i]),
							bind->len
					      );
				else
				{
					if(VARATT_IS_SHORT(DatumGetPointer(values[i])))
					{
						attr_len = VARSIZE_SHORT(DatumGetPointer(values[i]));
						Assert(attr_len <= bind->len);
						memcpy(memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp),
								DatumGetPointer(values[i]),
								attr_len
						      );
					}
					else
					{
						char *p = memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp);
						Assert(VARATT_COULD_SHORT_D(values[i]));
						attr_len = VARSIZE_D(values[i]) - VARHDRSZ + VARHDRSZ_SHORT;
						Assert(attr_len <= bind->len);
						*p = VARSIZE_TO_SHORT_D(values[i]);
						memcpy(p+1, VARDATA_D(values[i]), attr_len-1);
					}
				}
				break;
			case MTB_ByRef:
				if(VARATT_IS_EXTERNAL_D(values[i]))
				{
					varlen_start = (char *) att_align((long) varlen_start, attr->attalign);
					attr_len = VARSIZE_EXTERNAL(DatumGetPointer(values[i]));
					Assert((varlen_start - (char *) mtup) + attr_len <= len);
					memcpy(varlen_start, DatumGetPointer(values[i]), attr_len);
				}
				else if(VARATT_IS_SHORT_D(values[i]))
				{
					Assert(typid_could_short(attr->atttypid));
					attr_len = VARSIZE_SHORT(DatumGetPointer(values[i]));
					Assert((varlen_start - (char *) mtup) + attr_len <= len);
					memcpy(varlen_start, DatumGetPointer(values[i]), attr_len);
				}
				else if(VARATT_COULD_SHORT_D(values[i]) && typid_could_short(attr->atttypid))
				{
					attr_len = VARSIZE_D(values[i]) - VARHDRSZ + VARHDRSZ_SHORT;
					*varlen_start = VARSIZE_TO_SHORT_D(values[i]);
					Assert((varlen_start - (char *) mtup) + attr_len <= len);
					memcpy(varlen_start+1, VARDATA_D(values[i]), attr_len-1);
				}
				else
				{
					/* Must be 4 byte header aligned varlena */
					varlen_start = (char *) att_align((long) varlen_start, attr->attalign);
					attr_len = VARSIZE(DatumGetPointer(values[i]));
					Assert((varlen_start - (char *) mtup) + attr_len <= len);
					memcpy(varlen_start, DatumGetPointer(values[i]), attr_len);
				}

				*(short *) memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp) = (short) (varlen_start - start);
				varlen_start += attr_len;
				break;

			case MTB_ByRef_CStr:
				varlen_start = (char *) att_align((long) varlen_start, attr->attalign);
				attr_len = strlen(DatumGetCString(values[i])) + 1;
				Assert((varlen_start - (char *) mtup) + attr_len <= len);
				memcpy(varlen_start, DatumGetPointer(values[i]), attr_len);
				
				*(short *) memtuple_get_attr_ptr(start, bind, pbind->null_saves, nullp) = (short) (varlen_start - start);
				varlen_start += attr_len;
				break;
			default:
				Assert(!"Not valid binding type");
				break;
		}
	}

	Assert((varlen_start - (char *) mtup) <= len);
	return mtup;
}

bool memtuple_attisnull(MemTuple mtup, MemTupleBinding *pbind, int attnum)
{
	Assert(mtup && pbind && pbind->tupdesc);
	Assert(attnum > 0 && attnum <= pbind->tupdesc->natts);

	if(!memtuple_get_hasnull(mtup, pbind))
		return false;

	return (mtup->PRIVATE_mt_bits[pbind->bindings[attnum-1].null_byte] & pbind->bindings[attnum-1].null_mask);
}
				
Datum memtuple_getattr(MemTuple mtup, MemTupleBinding *pbind, int attnum, bool *isnull)
{
	bool hasnull = memtuple_get_hasnull(mtup, pbind);
	unsigned char *nullp = (hasnull ? mtup->PRIVATE_mt_bits : NULL);
	char *start = (char *) mtup + (hasnull ? pbind->null_bitmap_extra_size : 0);
	Datum ret;
	MemTupleAttrBinding *attrbind;

	Assert(mtup && pbind && pbind->tupdesc);
	Assert(attnum > 0 && attnum <= pbind->tupdesc->natts);

	if(isnull)
		*isnull = false;

	/* input attnum is 1 based.  Make it 0 based */
	--attnum;
	attrbind = &(pbind->bindings[attnum]);
	
	/* null check */
	if(hasnull && (mtup->PRIVATE_mt_bits[attrbind->null_byte] & attrbind->null_mask))
	{
		if(isnull)
			*isnull = true;
		return 0;
	}

	ret = fetchatt(pbind->tupdesc->attrs[attnum], memtuple_get_attr_data_ptr(start, attrbind, pbind->null_saves, nullp));

	return ret;
}

MemTuple memtuple_copy_to(MemTuple mtup, MemTupleBinding *pbind, MemTuple dest, uint32 *destlen)
{
	uint32 len = memtuple_get_size(mtup, pbind);

	if(!destlen)
		dest = (MemTuple) palloc(len);
	else
	{
		if(*destlen < len)
		{
			*destlen = len;
			return NULL;
		}

		*destlen = len;
	}

	memcpy((char *) dest, (char *) mtup, len);
	return dest;
}

