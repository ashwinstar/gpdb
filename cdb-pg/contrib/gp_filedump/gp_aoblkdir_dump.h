/*
 * gp_aoblkdir_dump.h
 */

#ifndef _GP_AOBLKDIR_DUMP_H_
#define _GP_AOBLKDIR_DUMP_H_

#include "c.h"

#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>

#include "postgres.h"
#include "pg_config_manual.h"

#include "storage/bufpage.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/gin.h"
#include "access/gist.h"
#include "access/nbtree.h"
#include "access/itup.h"
#include "access/htup.h"
#include "catalog/pg_control.h"
#include "utils/builtins.h"
#include "cdb/cdbappendonlyblockdirectory.h"
#include "fmgr.h"

/* This union is originally defined in tupletoaster.c.
 * May be this is an ugly way to program something. But to keep
 * this tool out of the backend area, this ugly path is chosen.
 * For details related to this struct, please refer tupletoaster.c */

typedef union varattrib
{
	struct						/* Normal varlena (4-byte length) */
	{
		uint8 	va_header;
		char 	va_data[1];
	} va_1byte;
	struct
	{
		uint8 	va_header;
		uint8	va_padding[3];
		int32	va_rawsize;		/* Plain data size */
		int32	va_extsize;		/* External saved size */
		Oid		va_valueid;		/* Unique identifier of value */
		Oid		va_toastrelid;	/* RelID where to find chunks */
	} va_external;
	struct						/* Compressed-in-line format */
	{
		uint32 	va_header;
		uint32	va_rawsize;		/* Original data size (excludes header) */
		char	va_data[1];		/* Compressed data */
	} va_compressed;
	struct
	{
		uint32 	va_header;
		char 	va_data[1];
	} va_4byte;
} varattrib;

extern void      gp_aoblkdir_dump(char *, TupleDesc);
extern TupleDesc gp_aoblkdir_dump_build_tupdesc(void);

#endif /* _GP_AOBLKDIR_DUMP_H_ */
