/*
 * gp_aoblkdir_dump.c
 * Function definitions to generate block directory relation tuple desc and
 * dump a block directory tuple along with minipage entries.
 *
 * Note :- There is no provision made to ignore invisible tuples.
 * All tuples will be displayed regardless of their transaction Id commit
 * status.
 */


/* Note about malloc() :- malloc is used instead of palloc because palloc calls some functions which 
 * have dependencies on some other functions and thus there is no good way to link all such source files
 * outside the main backend code (we are currently outside the main backend code (contrib)).
 * */

#include "gp_aoblkdir_dump.h"
#define numOfAtt 4 /*Block directory tuple has 4 columns*/

/*Local functions*/
static bool get_minipage(MinipagePerColumnGroup *, Datum, bool);

/* Builds a tupledesc for a tuple in a block directory relation.
 * Returns a built tuple desc. The caller has to take care of freeing
 * the returned tuple descriptor.*/
TupleDesc
gp_aoblkdir_dump_build_tupdesc()
{
	/* In coming future there is a little chance of AO Block directory format being changed. As a
	 * result the tupdesc will be hard coded instead of preparing it dynamically from pg_attribute.
	 * As of now it seems worthless to generate the tupdesc on the run by reading pg_attribute for
	 * AO blk directory. */

	/*
	 * AO blk directory table attributes are described as follows. [From pg_atrributes]
	 attrelid |    attname     | atttypid | attstattarget | attlen | attnum | attndims | attcacheoff | atttypmod | attbyval | attstorage | attalign | attnotnull | atthasdef | attisdropped | attislocal | attinhcount
	----------+----------------+----------+---------------+--------+--------+----------+-------------+-----------+----------+------------+----------+------------+-----------+--------------+------------+-------------
	    XXXXX | segno          |       23 |            -1 |      4 |      1 |        0 |          -1 |        -1 | t        | p          | i        | f          | f         | f            | t          |           0
	    XXXXX | columngroup_no |       23 |            -1 |      4 |      2 |        0 |          -1 |        -1 | t        | p          | i        | f          | f         | f            | t          |           0
	    XXXXX | first_row_no   |       20 |            -1 |      8 |      3 |        0 |          -1 |        -1 | t        | p          | d        | f          | f         | f            | t          |           0
	    XXXXX | minipage       |     1562 |            -1 |     -1 |      4 |        0 |          -1 |        -1 | f        | x          | i        | f          | f         | f            | t          |           0
	*/

	int i =0;
	Form_pg_attribute *attrs;
	FormData_pg_attribute att_array[numOfAtt] = {  {1,{"segno"},         23,  -1, 4,1,0,-1,-1,1,'p','i',0,0,0,1,0},
                                                 {1,{"columngroup_no"},23,  -1, 4,2,0,-1,-1,1,'p','i',0,0,0,1,0},
                                                 {1,{"first_row_no"},  20,  -1, 8,3,0,-1,-1,1,'p','d',0,0,0,1,0},
                                                 {1,{"minipage"},      1562,-1,-1,4,0,-1,-1,0,'x','i',0,0,0,1,0} };
	TupleDesc tupDesc = NULL;

	/*4 columns only*/
	attrs = malloc(sizeof(Form_pg_attribute) * numOfAtt);
	Assert(attrs != NULL);

	for (i=0;i<numOfAtt;i++)
	{
		attrs[i] = (Form_pg_attribute)malloc(sizeof(FormData_pg_attribute));
		Assert(attrs[i]);
		*(attrs[i]) = att_array[i];
	}

	/*Build tuple desc*/
	tupDesc = malloc(sizeof(TupleDesc));
	Assert(tupDesc != NULL);
	tupDesc->natts = numOfAtt;
	tupDesc->attrs = attrs;

	return tupDesc;
}

/* Dumps out a block directory tuple along with minipage entries(if possible).
 * This function takes a buffer which has tuple stored. A tuple desc of the
 * block directory should be supplied along with the buffer. Anything invalid
 * will result into wrong output. The caller should make sure to call
 * gp_aoblkdir_dump_build_tupdesc() to get appropriate tupdesc.*/
void
gp_aoblkdir_dump(char *buffer, TupleDesc tupDesc)
{
	Assert(buffer != NULL && tupDesc != NULL);
	int nAtts =0;
	AttrNumber      attNo;
	HeapTupleHeader hTup = NULL;
	HeapTuple tuple = NULL;

	/*Using tuple and tupdesc print all its contents*/
	hTup = (HeapTupleHeader) buffer;
	nAtts = HeapTupleHeaderGetNatts(hTup);

	tuple = malloc(sizeof(HeapTupleData));
	Assert(tuple);
	tuple->t_data = hTup;

	for (attNo = 1; attNo <= nAtts; attNo++)
	{
		Form_pg_attribute att = tupDesc->attrs[attNo - 1];
		bool            isNull;
		Datum           datum = heap_getattr(tuple, attNo, tupDesc, &isNull);

		if (isNull)
			printf("\nNULL VALUE\n");
		else
		{
			switch (att->atttypid)
			{
				case INT4OID:
					printf("%11d  ",DatumGetInt32(datum));
					break;

				case INT8OID:
					printf("%20lld  ",DatumGetInt64(datum));
					break;

				case VARBITOID:
	     		/*Minipages have bit varying data type*/
				{
					MinipagePerColumnGroup *miniPageInfo = NULL;
					int idx = 0;

					miniPageInfo = malloc (sizeof(MinipagePerColumnGroup));
					Assert(miniPageInfo);
					miniPageInfo->minipage = malloc(offsetof(Minipage, entry) +
                    								    sizeof(MinipageEntry) * NUM_MINIPAGE_ENTRIES);
          Assert(miniPageInfo->minipage);
          
					miniPageInfo->numMinipageEntries = 0;

					/*See if we can extract the minipage from the tuple itself*/
					if(!get_minipage(miniPageInfo, datum, isNull))
						goto MinipageEnd;

					/*Yes, we got the minipage from the tuple itself*/
					printf("%10s"," ");
					printf("Minipage Num of Entries = %3d\n\n", miniPageInfo->minipage->nEntry);
					printf("%57s"," ");
					printf("        FileOffset          FirstRowNum              RowCount\n");
					printf("%57s"," ");
					printf("---------------------------------------------------------------------\n");

					/*Now, we have got the mini page. Dump it out.*/
					for (idx=0;idx<miniPageInfo->numMinipageEntries;idx++)
					{
						printf("%57s"," ");
						printf("%3d] %020lld,%020lld,%020lld\n",idx + 1,miniPageInfo->minipage->entry[idx].fileOffset,
								  miniPageInfo->minipage->entry[idx].firstRowNum,
								  miniPageInfo->minipage->entry[idx].rowCount  );
					}

					printf("\n");

MinipageEnd:
					free(miniPageInfo->minipage);
					free(miniPageInfo);
				}
						break;
				default:
					printf("\nSomething is WRONG!");
					break;
			}
		}
	}

  /*Cleanup*/
  free(tuple);
}


/* Redundant code of copy_out_minipage(in appendonlyblockdirectory.c).
 * No great way to implement this. But did not want to make the original call open
 * to public. Any suggestions ?*/
static bool
get_minipage(MinipagePerColumnGroup *minipageInfo,
			 Datum minipage_value,
			 bool minipage_isnull)
{
	struct varlena *attr  = NULL;
	struct varlena *value = NULL ;

	Assert(!minipage_isnull);

	value = (struct varlena *)
		DatumGetPointer(minipage_value);

	/*The minipage attribute can be either extended or simple i.e can be directly
	 * extracted without any further processing*/
	if (VARATT_IS_EXTENDED(value))
	{
		/* Limitation :- Cases where the minipage may reside outside the block
		 * directory relation file, will be ignored. Same rule applies if minipage
		 * data is compressed. This is to maintain simplicity and
		 * based on some results it has been observed that these two case may not
		 * occur (the probability is low!) */
		if (VARATT_IS_EXTERNAL(value))
		{
			printf("External stored Minipage!\n");
			return false;
		}

		if (VARATT_IS_COMPRESSED(value))
		{
			printf("Compressed Minipage!\n");
			return false;
		}
		else if (VARATT_IS_SHORT(value))
		{
			attr = value;
			Size 	data_size = VARSIZE_SHORT(attr);
			Size 	new_size = data_size - VARHDRSZ_SHORT + VARHDRSZ;
			varattrib *tmp = (varattrib *)attr;

			attr = (struct varlena *) malloc(new_size);
			Assert(attr);
			SET_VARSIZE(attr, new_size);
			memcpy(VARDATA(attr), VARDATA_SHORT(tmp), data_size - VARHDRSZ_SHORT);
		}
	}
	else
		attr = value;

	Assert( VARSIZE(attr) <= (offsetof(Minipage, entry) +
		    sizeof(MinipageEntry) * NUM_MINIPAGE_ENTRIES));

	memcpy(minipageInfo->minipage, attr, VARSIZE(attr));

	if (attr != value)
		free(attr);

	Assert(minipageInfo->minipage->nEntry <= NUM_MINIPAGE_ENTRIES);
	minipageInfo->numMinipageEntries = minipageInfo->minipage->nEntry;

	return true;
}
