/*-------------------------------------------------------------------------
 * cdbtmlog.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBTMLOG_H
#define CDBTMLOG_H

#include "utils/pg_crc.h"
#include "cdb/cdbtm.h"

/* recType	states */
typedef enum
{
	TM_LOG_XACT,				/* tm log */
	TM_LOG_CHECKPOINT,			/* checkpoint */
}	TmLogRecType;;

/*
 * Header for each record in tm log
 */
typedef struct TmLogRecordHeader
{
	uint32		thisFileNo;		/* This file no */
	uint32		thisOffset;		/* This Offset */
	char		gid[TMGIDSIZE]; /* gid */
	TmLogRecType recType;		/* record type - xact log, checkpoint log */
	DtxState	state;			/* xact state */
	uint32		numberOfParticipants;	/* number of participants */
}	TmLogRecordHeader;

typedef struct TmLogRecord
{
	TmLogRecordHeader header;

	/* PARTICIPANTS FOLLOWS AT END OF STRUCT */
	int			participants[0];
}	TmLogRecord;

typedef struct TmLogFileControlBlock
{
	uint32		magic;
	uint32		checkpointFileNo;
	uint32		checkpointOffset;
	crc64		crc;
}	TmLogFileControlBlock;

extern void writeTmLogRecord(TmLogRecord *);
extern void recoverTmLog(void);
extern void checkPointTm(void);
extern void flushTmLogFile(void);
extern void write_to_mirror(void *, int, int);
extern void write_with_ereport(int, void *, int);
extern int	initTmLog(char *);
extern void redoTmLog(TmLogRecord *);
extern bool openTmLogFile(int);
extern bool resync_tmlog_to_mirror(int, int);
extern TmLogRecord *readTmLogRecordNext(int offset);
TmLogFileControlBlock *getTmLogFileHeader();

#endif   /* CDBTMLOG_H */
