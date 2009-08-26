/*
 * Datum stream
 *
 *	Copyright (c) 2009, Greenplum Inc.
 */

#include "postgres.h"

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/tupmacs.h"
#include "cdb/cdbappendonlystorageread.h"
#include "cdb/cdbappendonlystoragewrite.h"
#include "utils/datumstream.h"

typedef enum AOCSBK
{
	AOCSBK_None = 0, 
    AOCSBK_BLOCK,
    AOCSBK_BLOB,
} AOCSBK; 


int datumstream_get(DatumStream *acc, Datum *datum, bool *null)
{
    Assert(acc && datum && null);
    Assert(acc->is_reader);

    if(acc->has_null && (*acc->nullp & acc->nullbit))
    {
        *null = true;
        return 0;
    }
    else 
        *null = false;

    if (!acc->byval) 
    {
        *datum = PointerGetDatum(acc->datump);
        AssertImply(acc->datumlen < 0, VARATT_IS_SHORT_D(*datum) || !VARATT_IS_EXTENDED_D(*datum));
    }
    else 
    {
        if(acc->datumlen == 1)
            *datum = *(uint8 *) acc->datump;
        else if(acc->datumlen == 2)
        {
            Assert(IsAligned(acc->datump, 2)); 
            *datum = *(uint16 *) acc->datump;
        }
        else if(acc->datumlen == 4)
        {
            Assert(IsAligned(acc->datump, 4)); 
            *datum = *(uint32 *) acc->datump;
        }
        else if(acc->datumlen == 8)
        {
            Assert(IsAligned(acc->datump, 8)); 
            *datum = *(Datum *) acc->datump;
        }
        else
        {
            Assert(!"A byval type has strange datumlen");
        }
    }

    return 0;
}

extern int datumstream_advance(DatumStream *acc)
{
    bool advdatump = true;

    Assert(acc);
    Assert(acc->is_reader);
    AssertImply(acc->block, acc->nth <= acc->block->ndatum);

    ++acc->nth;
    if (acc->nth == 0)
        return 1;

    /* Advance out of bound */
    if(!acc->block || acc->nth >= acc->block->ndatum)
        return 0;

    if(acc->has_null)
    {
        if(*acc->nullp & acc->nullbit)
            advdatump = false;

        acc->nullbit <<= 1;
        if(acc->nullbit == 0)
        {
            ++acc->nullp;
            acc->nullbit = 1;
        }
    }

    if(!advdatump)
        return 1;

    if(acc->datumlen < 0)
    {
        struct varlena *s = (struct varlena *) acc->datump;

        Assert(acc->datumlen == -1);
        acc->datump += VARSIZE_ANY(s);

        /* Might be zero paddings */
        if(*acc->datump == 0)
            acc->datump = (char *) att_align(acc->datump, acc->align);

        Assert((acc->has_null && (*acc->nullp & acc->nullbit))
                || VARATT_IS_SHORT_D(PointerGetDatum(acc->datump)) 
                || !VARATT_IS_EXTENDED_D(PointerGetDatum(acc->datump)) 
              ); 
    }
    else
        acc->datump += acc->datumlen; 

    return 1;
}

static inline void incr_acc_n(DatumStream *acc)
{
    acc->nullbit <<= 1;

    if(acc->nullbit == 0)
    {
        ++acc->nullp;
        acc->nullbit = 1;
    }

    ++acc->nth;
}

static bool datumstream_has_space(DatumStream *ds, bool null, int32 sz) 
{
    int32 dssz = (ds->datump - ds->dpbuf);

    if(ds->nth >= MAXDATUM_PER_AOCS_BLOCK)
        return false;

    if(null || ds->has_null)
        dssz += (ds->nullp - ds->npbuf);

    return dssz + sz < ds->maxdatasz;
}

int datumstream_put(DatumStream *acc, Datum d, bool null)
{
    Assert(acc && !acc->is_reader);
    Assert(acc->maxdatasz > 0 && acc->maxdatasz < acc->maxsz);

    if (!datumstream_has_space(acc, null, 0))
        return -1;

    if(null)
    {
        acc->has_null = true;
        *acc->nullp |= acc->nullbit;
        incr_acc_n(acc);
        return 0;
    }

    /* Varlen */
    if(acc->datumlen < 0)
    {
        int32 sz = 0;
        char *p = NULL;
        char c1 = 0;
        int32 wsz = 0;

        Assert(acc->datumlen == -1);
        Assert(!VARATT_IS_EXTERNAL_D(d));

        if(VARATT_IS_EXTERNAL_D(d))
        {
            Assert(!"AOCS NYI");
        }
        else if(VARATT_IS_SHORT_D(d))
        {
            sz = VARSIZE_SHORT(DatumGetPointer(d));
            p = DatumGetPointer(d);
            wsz = sz;
        }
        else if(value_type_could_short(d, acc->typid))
        {
            sz = VARSIZE_D(d) - VARHDRSZ + VARHDRSZ_SHORT;
            c1 = VARSIZE_TO_SHORT_D(d);
            p = VARDATA_D(d);
            wsz = sz-1;
        }
        else
        {
            sz = VARSIZE(DatumGetPointer(d));
            acc->datump = att_align_zero(acc->datump, acc->align);
            p = DatumGetPointer(d);
            wsz = sz;
        }

        if (!datumstream_has_space(acc, false, sz))
            return -sz;

        if (c1 != 0)
        {
            *(acc->datump) = c1;
            acc->datump += 1;
        }
        
        memcpy(acc->datump, p, wsz);
        acc->datump += wsz;
        incr_acc_n(acc);
        return sz;
    }

    /* Fixed len */
    if (!datumstream_has_space(acc, false, acc->datumlen))
        return -acc->datumlen;

    if (!acc->byval)
    {
        memcpy(acc->datump, DatumGetPointer(d), acc->datumlen);
        acc->datump += acc->datumlen;
    }
    else
    {
        switch(acc->datumlen)
        {
            case 1:
                *(acc->datump) = DatumGetChar(d);
                ++acc->datump;
                break;
            case 2:
                Assert(IsAligned(acc->datump, 2));
                *(uint16 *) (acc->datump) = DatumGetUInt16(d);
                acc->datump += 2;
                break;
            case 4:
                Assert(IsAligned(acc->datump, 4));
                *(uint32 *) (acc->datump) = DatumGetUInt32(d);
                acc->datump += 4;
                break;
            case 8:
                Assert(IsAligned(acc->datump, 8));
                *(Datum *) (acc->datump) = d;
                acc->datump += 8;
                break;
            default:
                Assert(!"Byval len type has strange length");
                break;
        }
    }

    incr_acc_n(acc);
    return acc->datumlen;
}

DatumStream *create_datumstream(CompressorType compType, int32 compLevel, int32 maxsz,  
        Form_pg_attribute attr, char *relname, bool isreader) 
{
    DatumStream *acc = palloc0(sizeof(DatumStream));

    /* datumlen and align not set */
    acc->datumlen = attr->attlen;
    acc->typid = attr->atttypid;
    acc->align = attr->attalign;
    acc->byval = attr->attbyval;

    /* Data type sepcific compression NYI */
    acc->compress_option = 0;

    acc->maxsz = maxsz;
    acc->is_reader = isreader;

    acc->ao_attr.compress = (compLevel > 0);
    acc->ao_attr.compressType = compType;
    acc->ao_attr.compressLevel = compLevel;

    if (acc->is_reader)
    {
        acc->ao_read = palloc0(sizeof(AppendOnlyStorageRead));
        AppendOnlyStorageRead_Init(acc->ao_read, NULL, maxsz, relname, &acc->ao_attr);
    }
    else
    {
        acc->ao_write = palloc0(sizeof(AppendOnlyStorageWrite));
        AppendOnlyStorageWrite_Init(acc->ao_write, NULL, maxsz, relname, &acc->ao_attr);

        acc->npbufsz = MAXDATUM_PER_AOCS_BLOCK / 8;
        acc->npbuf = palloc0(acc->npbufsz);

        acc->dpbufsz = acc->maxsz;
        acc->dpbuf = palloc0(acc->dpbufsz);

        acc->lobbufsz = 0;
        acc->lobbuf = NULL;
    }

    acc->memctxt = CurrentMemoryContext;

    /* Set up current position */
    acc->nth = 0;
    acc->has_null = false;
    acc->nullp = acc->npbuf;
    acc->nullbit = 1;
    acc->datump = acc->dpbuf;

    return acc;
}

void destroy_datumstream(DatumStream *ds)
{
    if (ds->npbuf)
        pfree(ds->npbuf);
    if (ds->dpbuf)
        pfree(ds->dpbuf);
    if (ds->lobbuf)
        pfree(ds->lobbuf);

    if (ds->ao_read)
    {
        Assert(ds->is_reader);
        Assert(!ds->ao_write);
        AppendOnlyStorageRead_FinishSession(ds->ao_read);
        pfree(ds->ao_read);
    }

    if (ds->ao_write)
    {
        Assert(!ds->is_reader);
        Assert(!ds->ao_read);
        AppendOnlyStorageWrite_FinishSession(ds->ao_write);
        pfree(ds->ao_write);
    }

    pfree(ds);
}

void datumstream_open_file(DatumStream *ds, char *fn, int64 eof, int64 eofUncompressed)
{
    ds->eof = eof;
    ds->eofUncompress = eofUncompressed;

    if (ds->is_reader)
    {
        Assert(ds->ao_read && !ds->ao_write);
        AppendOnlyStorageRead_OpenFile(ds->ao_read, fn, ds->eof);
    }
    else
    {
        Assert(ds->ao_write && !ds->ao_read);
        AppendOnlyStorageWrite_OpenFile(ds->ao_write, fn, eof, eofUncompressed);
        ds->maxdatasz = ds->maxsz - AppendOnlyStorageWrite_CompleteHeaderLen(ds->ao_write) - sizeof(DatumStreamBlock);
    }
}

void datumstream_close_file(DatumStream *ds)
{
    if (ds->is_reader)
    {
        Assert(ds->ao_read && !ds->ao_write);
        AppendOnlyStorageRead_CloseFile(ds->ao_read);
    }
    else
    {
        Assert(ds->ao_write && !ds->ao_read);
        AppendOnlyStorageWrite_FlushAndCloseFile(ds->ao_write,
            &ds->eof,
            &ds->eofUncompress
            );
    }
}
    
int64 datumstream_write_block(DatumStream *acc)
{
    int64 wsize = 0;
    char *buffer = NULL;
    char *p = NULL;
    DatumStreamBlock block;

    Assert(acc && !acc->is_reader && !acc->ao_read && acc->ao_write);

    /* compression NYI */
    Assert(acc->compress_option == 0);

    /* Nothing to write, this is just no op */
    if (acc->nth == 0)
        return 0;

    /* Has to got something to write */
    /* First fill in header info */
    buffer = (char *) AppendOnlyStorageWrite_GetBuffer(acc->ao_write);
    p = buffer;

    /* First write header */
    block.version = 0;
    block.flags = acc->has_null ? DSB_HAS_NULLBITMAP : 0;
    block.ndatum = acc->nth;

    /* compress null bitmaps */
    if(!acc->has_null)
        block.nullsz = 0;
    else
        block.nullsz = MAXALIGN((acc->nth+7) >> 3); 
    block.sz = acc->datump - acc->dpbuf;

    memcpy(p, &block, sizeof(DatumStreamBlock));
    p += sizeof(DatumStreamBlock);

    /* Next write the null bitmap */
    memcpy(p, acc->npbuf, block.nullsz);
    p += block.nullsz;

    /* Next write data */
    memcpy(p, acc->dpbuf, block.sz);
    p += block.sz;

    /* Write it out */
    AppendOnlyStorageWrite_FinishBuffer(acc->ao_write,
            p-buffer,
            AOCSBK_BLOCK,
            block.ndatum
            );

    /* Reset acc after write */
    acc->nth = 0;
    acc->has_null = false;
    acc->nullp = acc->npbuf;
    memset(acc->npbuf, 0, acc->npbufsz);
    acc->nullbit = 1;
    acc->datump = acc->dpbuf;

    return wsize; 
}

int64 datumstream_write_lob(DatumStream *acc, Datum d)
{
    struct varlena *va = (struct varlena *) DatumGetPointer(d);

    Assert(acc && !acc->is_reader && !acc->ao_read && acc->ao_write);

    AppendOnlyStorageWrite_Content(acc->ao_write,
        (uint8 *) va,
        VARSIZE_ANY(va),
        AOCSBK_BLOB,
        1
        );

    Assert(acc->nth == 0);
    Assert(acc->has_null == false);
    Assert(acc->nullp == acc->npbuf);
    Assert(acc->datump == acc->dpbuf);

    return VARSIZE_ANY(va); 
}

int datumstream_read_block(DatumStream *acc)
{
    int32 contentLen = 0;
    int execBlockKind = 0;
    int64 firstRow = 0;
    int rowCnt = 0;
    bool isLarge = 0;
    bool isCompressed = 0;
    char *buffer = NULL;

    bool readOK = false;
    
    Assert(acc && acc->is_reader && acc->ao_read && !acc->ao_write);
    
    readOK = AppendOnlyStorageRead_GetBlockInfo(acc->ao_read,
            &contentLen,
            &execBlockKind,
            &firstRow,
            &rowCnt,
            &isLarge,
            &isCompressed
            );

    if (!readOK)
        return -1;

    if (execBlockKind == AOCSBK_BLOCK) 
    {
        Assert(!isLarge);

        if (isCompressed)
        {
            /* Compressed, need to decompress to our own buffer.  */
            if (acc->lobbufsz < contentLen)
            {
                MemoryContext oldCtxt;
                oldCtxt = MemoryContextSwitchTo(acc->memctxt);

                if (acc->lobbuf)
                    pfree(acc->lobbuf);

                acc->lobbufsz = contentLen;
                acc->lobbuf = palloc(contentLen);
                MemoryContextSwitchTo(oldCtxt);
            }

            AppendOnlyStorageRead_Content(acc->ao_read,
                    (uint8 *) acc->lobbuf,
                    contentLen
                    );

            buffer = (char *) acc->lobbuf;
        }
        else
            buffer = (char *) AppendOnlyStorageRead_GetBuffer(acc->ao_read);

        acc->block = (DatumStreamBlock *) buffer;
        buffer += sizeof(DatumStreamBlock);

        /* Set up acc */
        acc->nth = -1; /* put it before first entry.  Caller will advance */ 
        acc->has_null = acc->block->flags & DSB_HAS_NULLBITMAP;
        acc->nullp = buffer; 
        acc->nullbit = 1;
        acc->datump = buffer + acc->block->nullsz; 
    }
    else
    {
        Assert(execBlockKind == AOCSBK_BLOB); 
        Assert(isLarge);
        Assert(rowCnt == 1);

        acc->block = NULL;

        if (acc->lobbufsz < contentLen)
        {
            MemoryContext oldCtxt;
            oldCtxt = MemoryContextSwitchTo(acc->memctxt);
            
            if (acc->lobbuf)
                pfree(acc->lobbuf);

            acc->lobbufsz = contentLen;
            acc->lobbuf = palloc(contentLen);
            MemoryContextSwitchTo(oldCtxt);
        }


        AppendOnlyStorageRead_Content(acc->ao_read,
                (uint8 *) acc->lobbuf,
                contentLen
                );

        acc->nth = -1;
        acc->has_null = false;
        acc->datump = acc->lobbuf;
    }

    return 0;
}

