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
#include "utils/datumstream.h"

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

        if (acc->datumlen < 0)
        {
		*datum = PointerGetDatum(acc->datump);
                Assert(VARATT_IS_SHORT_D(*datum) || !VARATT_IS_EXTENDED_D(*datum)); 
        }
	else if(acc->datumlen == 1)
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
		*datum = PointerGetDatum(acc->datump);

	return 0;
}

extern int datumstream_advance(DatumStream *acc)
{
        bool advdatump = true;

	Assert(acc);
	Assert(acc->is_reader);
        Assert(acc->nth <= acc->block.ndatum);

        ++acc->nth;
	/* Advance out of bound */
	if(acc->nth >= acc->block.ndatum)
                return 0;

        if(acc->nth == 0)
                return 1;

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

                Assert(acc->datump <= acc->dpbuf + acc->dpbufsz);
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


int datumstream_put(DatumStream *acc, Datum d, bool null)
{
	Assert(acc && !acc->is_reader);

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
                int32 sz;

                Assert(acc->datumlen == -1);
                Assert(!VARATT_IS_EXTERNAL_D(d));
               
                if(VARATT_IS_EXTERNAL_D(d))
                {
                        Assert(!"AOCS NYI");
                }
                else if(VARATT_IS_SHORT_D(d))
                {
                        sz = VARSIZE_SHORT(DatumGetPointer(d));
                        Assert(sz > 0 && sz < 128);
                        if(acc->datump - acc->dpbuf + sz > acc->dpbufsz)
                                return -sz;

                        memcpy(acc->datump, DatumGetPointer(d), sz);
                        acc->datump += sz;
                        incr_acc_n(acc);
                        return sz;
                }
                else if(value_type_could_short(d, acc->typid))
                {
                        sz = VARSIZE_D(d) - VARHDRSZ + VARHDRSZ_SHORT;
                        Assert(sz > 0 && sz < 128);
                        if(acc->datump - acc->dpbuf + sz > acc->dpbufsz)
                                return -sz;
                        *(acc->datump) = VARSIZE_TO_SHORT_D(d);
                        memcpy(acc->datump+1, VARDATA_D(d), sz-1);
                        acc->datump += sz;
                        incr_acc_n(acc);
                        return sz;
                }
                else
                {
                        sz = VARSIZE(DatumGetPointer(d));
                        
                        if(MAXALIGN(acc->datump - acc->dpbuf) + sz > acc->dpbufsz)
                                return -sz;

                        acc->datump = att_align_zero(acc->datump, acc->align);
                        memcpy(acc->datump, DatumGetPointer(d), sz);
                        acc->datump += sz;
                        incr_acc_n(acc);
                        return sz;
                }
        }

        /* Fixed len */
        if(acc->datump - acc->dpbuf + acc->datumlen > acc->dpbufsz)
                return -acc->datumlen;

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
                        memcpy(acc->datump, DatumGetPointer(d), acc->datumlen);
                        acc->datump += acc->datumlen;
                        break;
        }

        incr_acc_n(acc);
        return acc->datumlen;
}

DatumStream *create_datumstream(CompressorType compType, int32 compLevel, int32 maxsz, 
                Form_pg_attribute attr, char *fn, bool isreader) 
{
	DatumStream *acc = palloc0(sizeof(DatumStream));

	/* datumlen and align not set */
	acc->datumlen = attr->attlen;
        acc->typid = attr->atttypid;
	acc->align = attr->attalign;

	acc->maxsz = maxsz;

        acc->compType = compType;
        acc->compLevel = compLevel;

	/* For now, let's set comp_opt to 0, no data type specific compression. */
	acc->compress_option = 0;

	acc->is_reader = isreader;
        if(acc->is_reader)
                acc->f = PathNameOpenFile(fn, O_RDONLY | PG_BINARY, 0600);
        else
                acc->f = PathNameOpenFile(fn, O_RDWR | PG_BINARY | O_CREAT, 0600);

        if(acc->f < 0)
                ereport(ERROR, (errcode_for_file_access(),
                                 errmsg("AOCS cannot open file %s", fn)));

	/* Set up buffers */
	acc->npbufsz = MAXALIGN((acc->maxsz+7) >> 3);
	acc->npbuf = palloc0(acc->npbufsz);
	acc->dpbufsz = MAXALIGN(maxsz);
	acc->dpbuf = palloc(acc->dpbufsz);

        if(acc->compType != COMPTYPE_NONE)
        {
                /* NOTE: quick lz need a MAX_OVERRUN_QUICKLZ bytes overhead for compress, and
                 * need a scratch area. 
                 */
                int overheadsz = (acc->compType == COMPTYPE_ZLIB ? 0 : MAX_OVERRUN_QUICKLZ);
                acc->npzbufsz = acc->npbufsz + overheadsz;
                acc->npzbuf = palloc0(acc->npzbufsz);
                acc->dpzbufsz = acc->dpbufsz + overheadsz;
                acc->dpzbuf = palloc(acc->dpzbufsz);

                if(acc->compType == COMPTYPE_QUICKLZ1)
                {
                        acc->qlz_scratchsz = QLZ1_SCRATCH_COMPRESS;
                        acc->qlz_scratch = palloc(acc->qlz_scratchsz);
                }
                else if (acc->compType == COMPTYPE_QUICKLZ3)
                {
                        acc->qlz_scratchsz = QLZ3_SCRATCH_COMPRESS;
                        acc->qlz_scratch = palloc(acc->qlz_scratchsz);
                }
        }

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
        if(ds->f >= 0)
                FileClose(ds->f);

        if(ds->npbuf)
                pfree(ds->npbuf);

        if(ds->dpbuf)
                pfree(ds->dpbuf);

        if(ds->npzbuf)
                pfree(ds->npzbuf);

        if(ds->dpzbuf)
                pfree(ds->dpzbuf);

        if(ds->qlz_scratch)
                pfree(ds->qlz_scratch);

        pfree(ds);
}

static void ec_write(DatumStream *ds, char *buf, int wsz, int uncomp_sz)
{
	int wsize = 0;

        /* Compression NYI. */
        ds->eof += wsz;
        ds->eof_uncompressed += uncomp_sz;

	while(wsz > 0)
	{
		wsize = FileWrite(ds->f, buf, wsz);
		if(wsize < 0)
			elog(ERROR, "Datum stream write failed");
		wsz -= wsize;
	}
}

int64 datumstream_write_block(DatumStream *acc)
{
	int64 wsize = 0;
	Assert(acc);

	/* compression NYI */
	Assert(acc->compress_option == 0);

        /* Nothing to write, this is just no op */
        if(acc->nth == 0)
        {
                Assert(!"Should not hit me");
                return 0;
        }

	/* Has to got something to write */
	/* First fill in header info */
	acc->block.version = 0;
	acc->block.flags = acc->has_null ? DSB_HAS_NULLBITMAP : 0;
	acc->block.ndatum = acc->nth;

        /* compress null bitmaps */
        if(!acc->has_null)
        {
                acc->block.nullsz = 0;
                acc->block.nullsz2 = 0;
        }
        else
        {
                acc->block.nullsz = MAXALIGN((acc->nth+7) >> 3); 

                if(acc->compType == COMPTYPE_NONE)
                        acc->block.nullsz2 = acc->block.nullsz;
                else
                {
                        int err;
                        
                        acc->block.nullsz2 = acc->npzbufsz;
                        err = gp_trycompress(acc->compType, acc->compLevel, 
                                        acc->npbuf, acc->block.nullsz, 
                                        acc->npzbuf, &acc->block.nullsz2,
                                        acc->qlz_scratch
                                        );

                        /* Do not compress this */
                        if(err != Z_OK || acc->block.nullsz2 > acc->block.nullsz)
                                acc->block.nullsz2 = acc->block.nullsz;
                }
        }

        /* compress data */
        acc->block.sz = acc->datump - acc->dpbuf;
        if(acc->compType == COMPTYPE_NONE)
                acc->block.sz2 = acc->block.sz;
        else
        {
                int err;
                
                acc->block.sz2 = acc->dpzbufsz; 
                err = gp_trycompress(acc->compType, acc->compLevel, 
                                        acc->dpbuf, acc->block.sz, 
                                        acc->dpzbuf, &acc->block.sz2,
                                        acc->qlz_scratch
                                        );

                /* Do not compress this */
                if(err != Z_OK || acc->block.sz2 > acc->block.sz)
                        acc->block.sz2 = acc->block.sz;
        }

	/* First write header */
        Assert(acc->block.nullsz2 <= acc->block.nullsz);
        Assert(acc->block.sz2 <= acc->block.sz);
        Assert(acc->block.nullsz > 0 || acc->block.sz > 0);

	ec_write(acc, (char *) &acc->block, sizeof(DatumStreamBlock), sizeof(DatumStreamBlock));
	wsize = sizeof(DatumStreamBlock);

	/* If has null bit map, write nullbit map */
	if(acc->has_null)
	{
                if(acc->block.nullsz == acc->block.nullsz2)
                {
                        ec_write(acc, acc->npbuf, acc->block.nullsz2, acc->block.nullsz);
                        wsize += acc->block.nullsz;
                }
                else
                {
                        ec_write(acc, acc->npzbuf, acc->block.nullsz2, acc->block.nullsz);
                        wsize += acc->block.nullsz2;
                }
	}

        if(acc->block.sz > 0)
        {
                if(acc->block.sz == acc->block.sz2)
                {
                        ec_write(acc, acc->dpbuf, acc->block.sz2, acc->block.sz);
                        wsize += acc->block.sz;
                }
                else
                {
                        ec_write(acc, acc->dpzbuf, acc->block.sz2, acc->block.sz);
                        wsize += acc->block.sz2;
                }
        }

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
        int64 wsize = 0;

	/* compression NYI */
	Assert(acc->compress_option == 0);

	/* First fill in header info */
	acc->block.version = 0;
	acc->block.flags = 0;
	acc->block.ndatum = 1;
	acc->block.nullsz = 0;
        acc->block.nullsz2 = 0;

	/* Compression NYI.  Everything is simple for now */
	acc->block.sz = VARSIZE_ANY(va);
        acc->block.sz2 = acc->block.sz;

        /* Write header */
	ec_write(acc, (char *) &acc->block, sizeof(DatumStreamBlock), sizeof(DatumStreamBlock));
        wsize += sizeof(DatumStreamBlock);

	/* Write data */
	ec_write(acc, (char *) va, acc->block.sz, acc->block.sz); 
        wsize += acc->block.sz; 

	/* Reset acc after write */
	acc->nth = 0;
	acc->has_null = false;
	acc->nullp = acc->npbuf;
        memset(acc->npbuf, 0, acc->npbufsz);
	acc->nullbit = 1;
	acc->datump = acc->dpbuf;

	return wsize; 
}

static int ec_read(DatumStream *ds, char *buf, int sz)
{
	int rsize;

        Assert(ds->reof <= ds->eof);
        if(ds->reof == ds->eof)
                return -1;

        if(ds->reof + sz > ds->eof)
                elog(ERROR, "Datum stream read out of bound");

        ds->reof += sz;

	while(sz > 0)
	{
		rsize = FileRead(ds->f, buf, sz);
                Assert(rsize >= 0); 
		sz -= rsize;
	}
        return 0;
}

int datumstream_read_block(DatumStream *acc)
{
        int err;
	Assert(acc);

	/* First readin the header */
	err = ec_read(acc, (char *) &acc->block, sizeof(DatumStreamBlock));
        if(err < 0)
                return -1;

	/* Then the null bitmap */
	if (acc->block.flags & DSB_HAS_NULLBITMAP)
	{
                Assert(acc->block.nullsz2 > 0);
                Assert(acc->block.nullsz >= acc->block.nullsz2);
                Assert(acc->block.nullsz <= acc->npbufsz);

                if (acc->block.nullsz == acc->block.nullsz2)
                {
                        /* not compressed */
                        err = ec_read(acc, acc->npbuf, acc->block.nullsz);
                        Assert(err >= 0);
                }
                else
                {
                        Assert(acc->compType != COMPTYPE_NONE);
                        err = ec_read(acc, acc->npzbuf, acc->block.nullsz2);
                        Assert(err >= 0);
                        gp_decompress(acc->compType, 
                                        acc->npzbuf, acc->block.nullsz2,
                                        acc->npbuf, acc->block.nullsz,
                                        acc->qlz_scratch
                                     );
                }
        }

        if(acc->block.sz > 0)
        {
                /* 
                 * First make sure our buffer is big enough.  We may run into this
                 * for blobs
                 */
                if (acc->block.sz > acc->dpbufsz)
                {
                        char *tmp;
                        acc->dpbufsz = acc->block.sz * 1.1;
                        tmp = palloc(acc->dpbufsz);
                        pfree(acc->dpbuf);
                        acc->dpbuf = tmp;
                }


                if (acc->block.sz2 == acc->block.sz)
                {
                        err = ec_read(acc, acc->dpbuf, acc->block.sz);
                        Assert(err >= 0);
                }
                else
                {
                        Assert(acc->compType != COMPTYPE_NONE);

                        if (acc->block.sz2 > acc->dpzbufsz)
                        {
                                char *tmp;
                                acc->dpzbufsz = acc->block.sz2 * 1.1;
                                tmp = palloc(acc->dpzbufsz);
                                pfree(acc->dpzbuf);
                                acc->dpzbuf = tmp;
                        }

                        err = ec_read(acc, acc->dpzbuf, acc->block.sz2);
                        Assert(err >= 0);
                        gp_decompress(acc->compType, 
                                        acc->dpzbuf, acc->block.sz2,
                                        acc->dpbuf, acc->block.sz,
                                        acc->qlz_scratch
                                     );
                }
        }

	/* Set up acc */
	acc->nth = -1; /* put it before first entry.  Caller will advance */ 
	acc->has_null = acc->block.flags & DSB_HAS_NULLBITMAP;
	acc->nullp = acc->npbuf;
	acc->nullbit = 1;
	acc->datump = acc->dpbuf;

        return 0;
}
