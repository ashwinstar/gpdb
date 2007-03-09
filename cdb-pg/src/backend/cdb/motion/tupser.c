/*-------------------------------------------------------------------------
 * tupser.c
 *	   Functions for serializing and deserializing a heap tuple.
 *
 * Copyright (c) 2005, Greenplum
 *
 * IDENTIFICATION:
 *	   $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "cdb/tupser.h"
#include "libpq/pqformat.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/date.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* A MemoryContext used within the tuple serialize code, so that freeing of
 * space is SUPAFAST.  It is initialized in the first call to InitSerTupInfo()
 * since that must be called before any tuple serialization or deserialization
 * work can be done.
 */
static MemoryContext s_tupSerMemCtxt = NULL;

static void addByteStringToChunkList(TupleChunkList tcList, char *data, int datalen);

#define addCharToChunkList(tcList, x) \
	do															\
	{															\
		char y = (x);											\
		addByteStringToChunkList((tcList), (char *)&y, sizeof(y));	\
	} while (0)

#define addInt32ToChunkList(tcList, x) \
	do															\
	{															\
		int32 y = (x);											\
		addByteStringToChunkList((tcList), (char *)&y, sizeof(y));	\
	} while (0)

static inline int32 stringInfoGetInt32(StringInfo str);

/* Look up all of the information that SerializeTuple() and DeserializeTuple()
 * need to perform their jobs quickly.	Also, scratchpad space is allocated
 * for serialization and desrialization of datum values, and for formation/
 * deformation of tuples themselves.
 *
 * NOTE:  This function allocates various data-structures, but it assumes that
 *		  the current memory-context is acceptable.  So the caller should set
 *		  the desired memory-context before calling this function.
 */
void
InitSerTupInfo(TupleDesc tupdesc, SerTupInfo * pSerInfo)
{
	int			i,
				numAttrs;

	AssertArg(tupdesc != NULL);
	AssertArg(pSerInfo != NULL);

	if (s_tupSerMemCtxt == NULL)
	{
		/* Create tuple-serialization memory context. */
		s_tupSerMemCtxt =
			AllocSetContextCreate(TopMemoryContext,
								  "TupSerMemCtxt",
								  ALLOCSET_DEFAULT_INITSIZE,	/* always have some memory */
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);
	}

	/* Set contents to all 0, just to make things clean and easy. */
	memset(pSerInfo, 0, sizeof(SerTupInfo));

	/* Store the tuple-descriptor so we can use it later. */
	pSerInfo->tupdesc = tupdesc;

	/*
	 * If we have some attributes, go ahead and prepare the information for
	 * each attribute in the descriptor.  Otherwise, we can return right away.
	 */
	numAttrs = tupdesc->natts;
	if (numAttrs <= 0)
		return;

	pSerInfo->myinfo = (SerAttrInfo *) palloc0(numAttrs * sizeof(SerAttrInfo));

	pSerInfo->values = (Datum *) palloc(numAttrs * sizeof(Datum));
	pSerInfo->nulls = (char *) palloc(numAttrs * sizeof(char));

	for (i = 0; i < numAttrs; i++)
	{
		SerAttrInfo *attrInfo = pSerInfo->myinfo + i;

		/*
		 * Get attribute's data-type Oid.  This lets us shortcut the comm
		 * operations for some attribute-types.
		 */
		attrInfo->atttypid = tupdesc->attrs[i]->atttypid;
		
		if (attrInfo->atttypid == RECORDOID)
			elog(ERROR,"Can't serialize transient record types");
		
		/*
		 * Ok, we want the Binary input/output routines for the type if they exist,
		 * else we want the normal text input/output routines.
		 * 
		 * User defined types might or might not have binary routines.
		 * 
		 * getTypeBinaryOutputInfo throws an error if we try to call it to get
		 * the binary output routine and one doesn't exist, so let's not call that.
		 */
		{
			HeapTuple	typeTuple;
			Form_pg_type pt;
		
			typeTuple = SearchSysCache(TYPEOID,
									   ObjectIdGetDatum(attrInfo->atttypid),
									   0, 0, 0);
			if (!HeapTupleIsValid(typeTuple))
				elog(ERROR, "cache lookup failed for type %u", attrInfo->atttypid);
			pt = (Form_pg_type) GETSTRUCT(typeTuple);
		
			if (!pt->typisdefined)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("type %s is only a shell",
								format_type_be(attrInfo->atttypid))));
								
			/* If we don't have both binary routines */
			if (!OidIsValid(pt->typsend) || !OidIsValid(pt->typreceive))
			{
				/* Use the normal text routines (slower) */
				if (!OidIsValid(pt->typoutput))
					ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no output function available for type %s",
								format_type_be(attrInfo->atttypid))));
				if (!OidIsValid(pt->typinput))
					ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("no input function available for type %s",
								format_type_be(attrInfo->atttypid))));
		
				attrInfo->typsend = pt->typoutput;
				attrInfo->send_typio_param = getTypeIOParam(typeTuple);
				attrInfo->typisvarlena = (!pt->typbyval) && (pt->typlen == -1);
				attrInfo->typrecv = pt->typinput;
				attrInfo->recv_typio_param = getTypeIOParam(typeTuple);
			}
			else
			{
				/* Use binary routines */
		
				attrInfo->typsend = pt->typsend;
				attrInfo->send_typio_param = getTypeIOParam(typeTuple);
				attrInfo->typisvarlena = (!pt->typbyval) && (pt->typlen == -1);
				attrInfo->typrecv  = pt->typreceive;
				attrInfo->recv_typio_param = getTypeIOParam(typeTuple);
			}
			
			ReleaseSysCache(typeTuple);
		}

		
		fmgr_info(attrInfo->typsend, &attrInfo->send_finfo);

		fmgr_info(attrInfo->typrecv, &attrInfo->recv_finfo);

#ifdef TUPSER_SCRATCH_SPACE

		/*
		 * If the field is a varlena, allocate some space to use for
		 * deserializing it.  If most of the values are smaller than this
		 * scratch-space then we save time on allocation and freeing.
		 */
		attrInfo->pv_varlen_scratch = palloc(VARLEN_SCRATCH_SIZE);
		attrInfo->varlen_scratch_size = VARLEN_SCRATCH_SIZE;
#endif
	}
}


/* Free up storage in a previously initialized SerTupInfo struct. */
void
CleanupSerTupInfo(SerTupInfo * pSerInfo)
{
	AssertArg(pSerInfo != NULL);

	/*
	 * Free any old data.
	 *
	 * NOTE:  This works because data-structure was bzero()ed in init call.
	 */

	if (pSerInfo->myinfo != NULL)
		pfree(pSerInfo->myinfo);
	pSerInfo->myinfo = NULL;

	if (pSerInfo->values != NULL)
		pfree(pSerInfo->values);
	pSerInfo->values = NULL;

	if (pSerInfo->nulls != NULL)
		pfree(pSerInfo->nulls);
	pSerInfo->nulls = NULL;

	pSerInfo->tupdesc = NULL;
}

/*
 * When manipulating chunks before transmit, it is important to notice that the
 * tcItem's chunk_length field *includes* the 4-byte chunk header, but that the
 * length within the header itself does not. Getting the two confused results
 * heap overruns and that way lies pain.
 */
static void
addByteStringToChunkList(TupleChunkList tcList, char *data, int datalen)
{
	TupleChunkListItem tcItem;
	int			remain,
				curSize,
				available,
				copyLen;
	char	   *pos;

	AssertArg(tcList != NULL);
	AssertArg(tcList->p_last != NULL);
	AssertArg(data != NULL);

	/* Add onto last chunk, lists always start with one chunk */
	tcItem = tcList->p_last;

	/* we'll need to add chunks */
	remain = datalen;
	pos = data;
	do
	{
		curSize = tcItem->chunk_length;

		available = tcList->max_chunk_length - curSize;
		copyLen = Min(available, remain);
		if (copyLen > 0)
		{
			/*
			 * make sure we don't stomp on the serialized header, chunk_length
			 * already accounts for it
			 */
			memcpy(tcItem->chunk_data + curSize, pos, copyLen);

			remain -= copyLen;
			pos += copyLen;
			tcList->serialized_data_length += copyLen;
			curSize += copyLen;

			SetChunkDataSize(tcItem->chunk_data, curSize - TUPLE_CHUNK_HEADER_SIZE);
			tcItem->chunk_length = curSize;
		}

		if (remain == 0)
			break;

		tcItem = (TupleChunkListItem)
			palloc0(sizeof(TupleChunkListItemData) + tcList->max_chunk_length);
		if (tcItem == NULL)
		{
			ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Could not allocate space for new chunk. %d of %d bytes in %d chunks", tcList->serialized_data_length, datalen, tcList->num_chunks)));
		}
		tcItem->chunk_length = TUPLE_CHUNK_HEADER_SIZE;
		SetChunkType(tcItem->chunk_data, TC_PARTIAL_MID);
		appendChunkToTCList(tcList, tcItem);
	} while (remain != 0);

	return;
}

typedef struct TupSerHeader
{
	uint32		tuplen;	
	uint16		natts;			/* number of attributes */
	uint16		infomask;		/* various flag bits */
} TupSerHeader;

/* Convert a HeapTuple into a byte-sequence, and store it directly into
 * a chunklist for transmission.
 *
 * This code is based on the printtup_internal_20() function in printtup.c.
 */
TupleChunkList
SerializeTupleIntoChunks(HeapTuple tuple, SerTupInfo * pSerInfo, int MaxChunkDataSize)
{
	TupleChunkList tcList = NULL;
	TupleChunkListItem tcItem = NULL;
	MemoryContext oldCtxt;
	TupleDesc	tupdesc;
	int			i,
		natts;
	bool		fHandled;

	AssertArg(tuple != NULL);
	AssertArg(pSerInfo != NULL);

	tupdesc = pSerInfo->tupdesc;
	natts = tupdesc->natts;

	/* get ready to go */
	tcList = (TupleChunkList) palloc0(sizeof(TupleChunkListData));
	if (tcList == NULL)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("Could not allocate space for new chunk list.")));
	}
	tcList->max_chunk_length = MaxChunkDataSize;

	if (natts == 0)
	{
		tcItem = (TupleChunkListItem)
			palloc0(sizeof(TupleChunkListItemData) + TUPLE_CHUNK_HEADER_SIZE);
		if (tcItem == NULL)
		{
			ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
							errmsg("Could not allocate space for first chunk item in new chunk list.")));
		}
		/* assume that we'll take a single chunk */
		SetChunkType(tcItem->chunk_data, TC_EMPTY);
		tcItem->chunk_length = TUPLE_CHUNK_HEADER_SIZE;
		appendChunkToTCList(tcList, tcItem);

		return tcList;
	}

	tcItem = (TupleChunkListItem)
		palloc0(sizeof(TupleChunkListItemData) + tcList->max_chunk_length);
	if (tcItem == NULL)
	{
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("Could not allocate space for first chunk item in new chunk list.")));
	}
	/* assume that we'll take a single chunk */
	SetChunkType(tcItem->chunk_data, TC_WHOLE);
	tcItem->chunk_length = TUPLE_CHUNK_HEADER_SIZE;
	appendChunkToTCList(tcList, tcItem);

	AssertState(s_tupSerMemCtxt != NULL);

	{
		TupSerHeader tsh;

		unsigned int	datalen;
		unsigned int	nullslen;

		HeapTupleHeader t_data = tuple->t_data;

		datalen = tuple->t_len - t_data->t_hoff;
		if (HeapTupleHasNulls(tuple))
			nullslen = BITMAPLEN(HeapTupleHeaderGetNatts(t_data));
		else
			nullslen = 0;

		tsh.tuplen = sizeof(TupSerHeader) + nullslen + datalen;
		tsh.natts = HeapTupleHeaderGetNatts(t_data);
		tsh.infomask = t_data->t_infomask;

		addByteStringToChunkList(tcList, (char *)&tsh, sizeof(TupSerHeader));
		/* If we don't have any attributes which have been toasted, we
		 * can be very very simple: just send the raw data. */
		if ((tsh.infomask & (HEAP_HASEXTERNAL | HEAP_HASEXTENDED)) == 0)
		{
			if (nullslen)
				addByteStringToChunkList(tcList, (char *)t_data->t_bits, nullslen);

			addByteStringToChunkList(tcList, (char *)t_data + t_data->t_hoff, datalen);
		}
		else
		{
			/* We have to be more careful when we have tuples that
			 * have been toasted. Ideally we'd like to send the
			 * untoasted attributes in as "raw" a format as possible
			 * but that makes rebuilding the tuple harder .
			 */
			oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);

			/* deconstruct the tuple (faster than a heap_getattr loop) */
			heap_deformtuple(tuple, tupdesc, pSerInfo->values, pSerInfo->nulls);

			MemoryContextSwitchTo(oldCtxt);

			/* Send the nulls character-array. */
			addByteStringToChunkList(tcList, pSerInfo->nulls, natts);

			/*
			 * send the attributes of this tuple: NOTE anything which allocates
			 * temporary space (e.g. could result in a PG_DETOAST_DATUM) should be
			 * executed with the memory context set to s_tupSerMemCtxt
			 */
			for (i = 0; i < natts; ++i)
			{
				SerAttrInfo *attrInfo = pSerInfo->myinfo + i;
				Datum		origattr = pSerInfo->values[i],
					attr;
				bytea	   *outputbytes=0;

				/* skip null attributes (already taken care of above) */
				if (pSerInfo->nulls[i] == 'n')
					continue;

				/*
				 * If we have a toasted datum, forcibly detoast it here to avoid
				 * memory leakage: we want to force the detoast allocation(s) to
				 * happen in our reset-able serialization context.
				 */
				if (attrInfo->typisvarlena)
				{
					oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);
					/* we want to detoast but leave compressed, if
					 * possible, but we have to handle varlena
					 * attributes (and others ?) differently than we
					 * currently do (first step is to use
					 * heap_tuple_fetch_attr() instead of
					 * PG_DETOAST_DATUM()). */
					attr = PointerGetDatum(PG_DETOAST_DATUM(origattr));
					MemoryContextSwitchTo(oldCtxt);
				}
				else
					attr = origattr;

				/*
				 * Assume that the data's output will be handled by the special IO
				 * code, and if not then we can handle it the slow way.
				 */
				fHandled = true;
				switch (attrInfo->atttypid)
				{
					case INT4OID:
						addInt32ToChunkList(tcList, DatumGetInt32(attr));
						break;
					case CHAROID:
						addCharToChunkList(tcList, DatumGetChar(attr));
						break;
					case BPCHAROID:
					case VARCHAROID:
					case INT2VECTOROID: /* postgres serialization logic broken, use our own */
					case OIDVECTOROID: /* postgres serialization logic broken, use our own */
					case ANYARRAYOID:
					{
						text	   *pText = DatumGetTextP(attr);
						int32		textSize = VARSIZE(pText) - VARHDRSZ;

						addInt32ToChunkList(tcList, textSize);
						addByteStringToChunkList(tcList, (char *) VARDATA(pText), textSize);
						break;
					}
					case DATEOID:
					{
						DateADT date = DatumGetDateADT(attr);

						addByteStringToChunkList(tcList, (char *) &date, sizeof(DateADT));
						break;
					}
					case NUMERICOID:
					{
						/*
						 * Treat the numeric as a varlena variable, and just push
						 * the whole shebang to the output-buffer.	We don't care
						 * about the guts of the numeric.
						 */
						Numeric		num = DatumGetNumeric(attr);
						int32		numSize = VARSIZE(num) - VARHDRSZ;

						addInt32ToChunkList(tcList, numSize);
						addByteStringToChunkList(tcList, (char *) VARDATA(num), numSize);
						break;
					}

					case ACLITEMOID:
					{
						AclItem		*aip = DatumGetAclItemP(attr);
						char		*outputstring;
						int32		aclSize ;

						outputstring = DatumGetCString(DirectFunctionCall1(aclitemout,
																		   PointerGetDatum(aip)));

						aclSize = strlen(outputstring);
						addInt32ToChunkList(tcList, aclSize);
						addByteStringToChunkList(tcList, outputstring,aclSize);
						break;
					}	

					case 210: /* storage manager */
					{
						char		*smgrstr;
						int32		strsize;

						smgrstr = DatumGetCString(DirectFunctionCall1(smgrout, 0));
						strsize = strlen(smgrstr);
						addInt32ToChunkList(tcList, strsize);
						addByteStringToChunkList(tcList, smgrstr, strsize);
						break;
					}

					default:
						fHandled = false;
				}

				if (fHandled)
					continue;

				/*
				 * the FunctionCall2 call into the send function may result in some
				 * allocations which we'd like to have contained by our reset-able
				 * context
				 */
				oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);						  
							  
				/* Call the attribute type's binary input converter. */
				if (attrInfo->send_finfo.fn_nargs == 1)
					outputbytes =
						DatumGetByteaP(FunctionCall1(&attrInfo->send_finfo,
													 attr));
				else if (attrInfo->send_finfo.fn_nargs == 2)
					outputbytes =
						DatumGetByteaP(FunctionCall2(&attrInfo->send_finfo,
													 attr,
													 ObjectIdGetDatum(attrInfo->send_typio_param)));
				else if (attrInfo->send_finfo.fn_nargs == 3)
					outputbytes =
						DatumGetByteaP(FunctionCall3(&attrInfo->send_finfo,
													 attr,
													 ObjectIdGetDatum(attrInfo->send_typio_param),
													 Int32GetDatum(tupdesc->attrs[i]->atttypmod)));
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("Conversion function takes %d args",attrInfo->recv_finfo.fn_nargs)));
				}
		
				MemoryContextSwitchTo(oldCtxt);

				/* We assume the result will not have been toasted */
				addInt32ToChunkList(tcList, VARSIZE(outputbytes) - VARHDRSZ);
				addByteStringToChunkList(tcList, VARDATA(outputbytes),
										 VARSIZE(outputbytes) - VARHDRSZ);

				/*
				 * this was allocated in our reset-able context, but we *are* done
				 * with it; and for tuples with several large columns it'd be nice to
				 * free the memory back to the context
				 */
				pfree(outputbytes);

			}

			MemoryContextReset(s_tupSerMemCtxt);
		}
	}

	/*
	 * if we have more than 1 chunk we have to set the chunk types on our
	 * first chunk and last chunk
	 */
	if (tcList->num_chunks > 1)
	{
		TupleChunkListItem first,
			last;

		first = tcList->p_first;
		last = tcList->p_last;

		Assert(first != NULL);
		Assert(first != last);
		Assert(last != NULL);

		SetChunkType(first->chunk_data, TC_PARTIAL_START);
		SetChunkType(last->chunk_data, TC_PARTIAL_END);

		/*
		 * any intervening chunks are already set to TC_PARTIAL_MID when
		 * allocated
		 */
	}

	return tcList;
}

/* Deserialize a HeapTuple's data from a byte-array.
 *
 * This code is based on the binary input handling functions in copy.c.
 */
HeapTuple
DeserializeTuple(SerTupInfo * pSerInfo, StringInfo serialTup)
{
	MemoryContext oldCtxt;
	TupleDesc	tupdesc;
	HeapTuple	htup;
	int			natts;
	SerAttrInfo *attrInfo;
	uint32		attr_size;

	int			i;
	int			origLen;
	StringInfoData attr_data;
	bool		fHandled;

	AssertArg(pSerInfo != NULL);
	AssertArg(serialTup != NULL);

	tupdesc = pSerInfo->tupdesc;
	natts = tupdesc->natts;
	origLen = serialTup->len;

	/*
	 * Flip to our tuple-serialization memory-context, to speed up memory
	 * reclamation operations.
	 */
	AssertState(s_tupSerMemCtxt != NULL);
	oldCtxt = MemoryContextSwitchTo(s_tupSerMemCtxt);

	/* Receive nulls character-array. */
	pq_copymsgbytes(serialTup, pSerInfo->nulls, natts);

	/* Deserialize the non-NULL attributes of this tuple */
	initStringInfo(&attr_data);
	for (i = 0; i < natts; ++i)
	{
		attrInfo = pSerInfo->myinfo + i;

		if (pSerInfo->nulls[i] == 'n')	/* NULL field. */
		{
			pSerInfo->values[i] = (Datum) 0;
			continue;
		}

		/*
		 * Assume that the data's output will be handled by the special IO
		 * code, and if not then we can handle it the slow way.
		 */
		fHandled = true;
		switch (attrInfo->atttypid)
		{
			case INT4OID:
				pSerInfo->values[i] = Int32GetDatum(stringInfoGetInt32(serialTup));
				break;

			case CHAROID:
				pSerInfo->values[i] = CharGetDatum(pq_getmsgbyte(serialTup));
				break;

			case BPCHAROID:
			case VARCHAROID:
			case INT2VECTOROID: /* postgres serialization logic broken, use our own */
			case OIDVECTOROID: /* postgres serialization logic broken, use our own */
			case ANYARRAYOID:
				{
					text	   *pText;
					int			textSize;

					textSize = stringInfoGetInt32(serialTup);

#ifdef TUPSER_SCRATCH_SPACE
					if (textSize + VARHDRSZ <= attrInfo->varlen_scratch_size)
						pText = (text *) attrInfo->pv_varlen_scratch;
					else
						pText = (text *) palloc(textSize + VARHDRSZ);
#else
					pText = (text *) palloc(textSize + VARHDRSZ);
#endif

					SET_VARSIZE(pText, textSize + VARHDRSZ);
					pq_copymsgbytes(serialTup, VARDATA(pText), textSize);
					pSerInfo->values[i] = PointerGetDatum(pText);
					break;
				}

			case DATEOID:
				{
					/*
					 * TODO:  I would LIKE to do something more efficient, but
					 * DateADT is not strictly limited to 4 bytes by its
					 * definition.
					 */
					DateADT date;

					pq_copymsgbytes(serialTup, (char *) &date, sizeof(DateADT));
					pSerInfo->values[i] = DateADTGetDatum(date);
					break;
				}

			case NUMERICOID:
				{
					/*
					 * Treat the numeric as a varlena variable, and just push
					 * the whole shebang to the output-buffer.	We don't care
					 * about the guts of the numeric.
					 */
					Numeric		num;
					int			numSize;

					numSize = stringInfoGetInt32(serialTup);

#ifdef TUPSER_SCRATCH_SPACE
					if (numSize + VARHDRSZ <= attrInfo->varlen_scratch_size)
						num = (Numeric) attrInfo->pv_varlen_scratch;
					else
						num = (Numeric) palloc(numSize + VARHDRSZ);
#else
					num = (Numeric) palloc(numSize + VARHDRSZ);
#endif

					SET_VARSIZE(num, numSize + VARHDRSZ);
					pq_copymsgbytes(serialTup, VARDATA(num), numSize);
					pSerInfo->values[i] = NumericGetDatum(num);
					break;
				}

			case ACLITEMOID:
				{
					int		aclSize, k, cnt;
					char		*inputstring, *starsfree;

					aclSize = stringInfoGetInt32(serialTup);
					inputstring = (char*) palloc(aclSize  + 1);
					starsfree = (char*) palloc(aclSize  + 1);
					cnt = 0;
	

					pq_copymsgbytes(serialTup, inputstring, aclSize);
					inputstring[aclSize] = '\0';
					for(k=0; k<aclSize; k++)
					{					
						if( inputstring[k] != '*')
						{
							starsfree[cnt] = inputstring[k];
							cnt++;
						}
					}
					starsfree[cnt] = '\0';

					pSerInfo->values[i] = DirectFunctionCall1(aclitemin, CStringGetDatum(starsfree));
					pfree(inputstring);
					break;
				}

			case 210:
				{
					int 		strsize;
					char		*smgrstr;

					strsize = stringInfoGetInt32(serialTup);
					smgrstr = (char*) palloc(strsize + 1);
					pq_copymsgbytes(serialTup, smgrstr, strsize);
					smgrstr[strsize] = '\0';

					pSerInfo->values[i] = DirectFunctionCall1(smgrin, CStringGetDatum(smgrstr));
					break;
				}
			default:
				fHandled = false;
		}

		if (fHandled)
			continue;

		attr_size = stringInfoGetInt32(serialTup);

		/* reset attr_data to empty, and load raw data into it */

		attr_data.len = 0;
		attr_data.data[0] = '\0';
		attr_data.cursor = 0;

		appendBinaryStringInfo(&attr_data,
							pq_getmsgbytes(serialTup, attr_size), attr_size);

		/* Call the attribute type's binary input converter. */
		if (attrInfo->recv_finfo.fn_nargs == 1)
			pSerInfo->values[i] = FunctionCall1(&attrInfo->recv_finfo,
											PointerGetDatum(&attr_data));
		else if (attrInfo->recv_finfo.fn_nargs == 2)
			pSerInfo->values[i] = FunctionCall2(&attrInfo->recv_finfo,
											PointerGetDatum(&attr_data),
							   ObjectIdGetDatum(attrInfo->recv_typio_param));
	    else if (attrInfo->recv_finfo.fn_nargs == 3)
	    	pSerInfo->values[i] = FunctionCall3(&attrInfo->recv_finfo,
											PointerGetDatum(&attr_data),
							   ObjectIdGetDatum(attrInfo->recv_typio_param),
							   Int32GetDatum(tupdesc->attrs[i]->atttypmod) );  
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("Conversion function takes %d args",attrInfo->recv_finfo.fn_nargs)));
		}

		/* Trouble if it didn't eat the whole buffer */
		if (attr_data.cursor != attr_data.len)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("incorrect binary data format")));
		}
	}

	/*
	 * Construct the tuple from the Datums and nulls values.  NOTE:  Switch
	 * out of our temporary context before we form the tuple!
	 */
	MemoryContextSwitchTo(oldCtxt);

	htup = heap_formtuple(tupdesc, pSerInfo->values, pSerInfo->nulls);

	MemoryContextReset(s_tupSerMemCtxt);

	/* All done.  Return the result. */
	return htup;
}

HeapTuple
CvtChunksToHeapTup(TupleChunkList tcList, SerTupInfo * pSerInfo)
{
	StringInfoData serData;
	TupleChunkListItem tcItem;
	int			i;
	HeapTuple	htup;

	AssertArg(tcList != NULL);
	AssertArg(tcList->p_first != NULL);
	AssertArg(pSerInfo != NULL);

	tcItem = tcList->p_first;

	if (tcList->num_chunks == 1 &&
		GetChunkType(tcItem) == TC_EMPTY)
	{
		/*
		 * the sender is indicating that there was a row with no attributes:
		 * return a NULL tuple
		 */
		clearTCList(tcList);

		htup = heap_formtuple(pSerInfo->tupdesc, pSerInfo->values, pSerInfo->nulls);

		return htup;
	}

	/*
	 * Dump all of the data in the tuple chunk list into a single StringInfo,
	 * so that we can convert it into a HeapTuple.	Check chunk types based on
	 * whether there is only one chunk, or multiple chunks.
	 *
	 * We know roughly how much space we'll need, allocate all in one go.
	 *
	 */
	initStringInfo(&serData);
	enlargeStringInfo(&serData, tcList->num_chunks * tcList->max_chunk_length);

	i = 0;
	do
	{
		/* Make sure that the type of this tuple chunk is correct! */

		if (i == 0)
		{
			if (tcItem->p_next == NULL)
			{
				if (GetChunkType(tcItem) != TC_WHOLE)
				{
					ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
									errmsg("Single chunk's type must be TC_WHOLE.")));
				}
			}
			else
				/* tcItem->p_next != NULL */
			{
				if (GetChunkType(tcItem) != TC_PARTIAL_START)
				{
					ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
									errmsg("First chunk of collection must have type"
										   " TC_PARTIAL_START.")));
				}
			}
		}
		else
			/* i > 0 */
		{
			if (tcItem->p_next == NULL)
			{
				if (GetChunkType(tcItem) != TC_PARTIAL_END)
				{
					ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
									errmsg("Last chunk of collection must have type"
										   " TC_PARTIAL_END.")));
				}
			}
			else
				/* tcItem->p_next != NULL */
			{
				if (GetChunkType(tcItem) != TC_PARTIAL_MID)
				{
					ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
									errmsg("Last chunk of collection must have type"
										   " TC_PARTIAL_MID.")));
				}
			}
		}

		/* Copy this chunk into the tuple data.  Don't include the header! */
		appendBinaryStringInfo(&serData,
							   (const char *) GetChunkDataPtr(tcItem) + TUPLE_CHUNK_HEADER_SIZE,
							   tcItem->chunk_length - TUPLE_CHUNK_HEADER_SIZE);

		/* Go to the next chunk. */
		tcItem = tcItem->p_next;
		i++;
	}
	while (tcItem != NULL);

	/* we've finished with the TCList, free it now. */
	clearTCList(tcList);

	{
		TupSerHeader *tshp;
		unsigned int	datalen;
		unsigned int	nullslen;
		unsigned int	hoff;
		HeapTupleHeader t_data;
		char *pos = (char *)serData.data;

		tshp = (TupSerHeader *)pos;
		pos += sizeof(TupSerHeader);	
		/* if the tuple had toasted elements we have to deserialize
		 * the old slow way. */
		if ((tshp->infomask & (HEAP_HASEXTERNAL | HEAP_HASEXTENDED)) != 0)
		{
			serData.cursor += sizeof(TupSerHeader);

			htup = DeserializeTuple(pSerInfo, &serData);

			/* Free up memory we used. */
			pfree(serData.data);
			return htup;
		}

		/* reconstruct lengths of null bitmap and data part */
		if (tshp->infomask & HEAP_HASNULL)
			nullslen = BITMAPLEN(tshp->natts);
		else
			nullslen = 0;
		datalen = tshp->tuplen - sizeof(TupSerHeader) - nullslen;

		/* determine overhead size of tuple (should match heap_form_tuple) */
		hoff = offsetof(HeapTupleHeaderData, t_bits) + nullslen;
		if (tshp->infomask & HEAP_HASOID)
			hoff += sizeof(Oid);
		hoff = MAXALIGN(hoff);

		/* Allocate the space in one chunk, like heap_form_tuple */
		htup = (HeapTuple)palloc(HEAPTUPLESIZE + hoff + datalen);

		t_data = (HeapTupleHeader) ((char *)htup + HEAPTUPLESIZE);

		/* make sure unused header fields are zeroed */
		MemSetAligned(t_data, 0, hoff);

		/* reconstruct the HeapTupleData fields */
		htup->t_len = hoff + datalen;
		ItemPointerSetInvalid(&(htup->t_self));
		htup->t_tableOid = InvalidOid;
		htup->t_data = t_data;

		/* reconstruct the HeapTupleHeaderData fields */
		ItemPointerSetInvalid(&(t_data->t_ctid));
		HeapTupleHeaderSetNatts(t_data, tshp->natts);
		t_data->t_infomask = tshp->infomask & ~HEAP_XACT_MASK;
		t_data->t_infomask |= HEAP_XMIN_INVALID | HEAP_XMAX_INVALID;
		t_data->t_hoff = hoff;

		if (nullslen)
		{
			memcpy((void *)t_data->t_bits, pos, nullslen);
			pos += nullslen;
		}

		/* does the tuple descriptor expect an OID ? Note: we don't
		 * have to set the oid itself, just the flag! (see heap_formtuple()) */
		if (pSerInfo->tupdesc->tdhasoid)		/* else leave infomask = 0 */
		{
			t_data->t_infomask |= HEAP_HASOID;
		}

		/* and now the data proper (it would be nice if we could just
		 * point our caller into our existing buffer in-place, but
		 * we'll leave that for another day) */
		memcpy((char *)t_data + hoff, pos, datalen);
	}

	/* Free up memory we used. */
	pfree(serData.data);

	return htup;
}

/*
 * stringInfoGetInt32
 *
 * Pull a 4-byte integer out of str, at the current cursor location.  The
 * cursor is then incremented by 4.  If there is not 4 bytes left between the
 * cursor and the end of the string, an error is reported.
 *
 * The input is expected to be in the current architecture's byte-ordering.
 *
 * Hopefully this is faster than the stuff in stringinfo.c or pqformat.c!
 */
static inline int32
stringInfoGetInt32(StringInfo str)
{
	int32		res;

	/* Make sure there are enough bytes left. */
	if (str->len - str->cursor < 4)
	{
		ereport(ERROR, (errcode(ERRCODE_PROTOCOL_VIOLATION),
						errmsg("deserialize data underflow")));
	}

	/*
	 * Pull out the int32.	We cast the pointer to the next part of the data
	 * to an int32 pointer, and just retrieve the integer from that location
	 * in the array.
	 */
	res = *((int32 *) (str->data + str->cursor));

	/* Update the cursor. */
	str->cursor += 4;

	return res;
}
