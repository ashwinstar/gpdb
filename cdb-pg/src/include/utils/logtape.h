/*-------------------------------------------------------------------------
 *
 * logtape.h
 *	  Management of "logical tapes" within temporary files.
 *
 * See logtape.c for explanations.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/logtape.h,v 1.15 2006/03/07 19:06:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LOGTAPE_H
#define LOGTAPE_H

/* LogicalTapeSet and LogicalTape are opaque types whose details are not known outside logtape.c. */
typedef struct LogicalTape LogicalTape;
typedef struct LogicalTapeSet LogicalTapeSet;

/*
 * prototypes for functions in logtape.c
 */

extern LogicalTape *LogicalTapeCreate(LogicalTapeSet *lts, LogicalTape *lt);
extern void LogicalTapeClose(LogicalTapeSet *lts, LogicalTape *lt, bool shouldFree);
extern LogicalTapeSet *LogicalTapeSetCreate(int ntapes);
extern LogicalTapeSet *LogicalTapeSetCreate_ReaderWriter(const char* rwfile_prefix, int ntapes); 
extern void LogicalTapeSetClose(LogicalTapeSet *lts);
extern void LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts);

extern size_t LogicalTapeReadTape(LogicalTapeSet *lts, LogicalTape *lt,
				void *ptr, size_t size);
extern size_t LogicalTapeRead(LogicalTapeSet *lts, int tapenum,
				void *ptr, size_t size);

extern void LogicalTapeWrite(LogicalTapeSet *lts, int tapenum,
				 void *ptr, size_t size);

extern void LogicalTapeRewindTape(LogicalTapeSet *lts, LogicalTape *lt, bool forWrite);
extern void LogicalTapeRewind(LogicalTapeSet *lts, int tapenum, bool forWrite);

extern void LogicalTapeFreeze(LogicalTapeSet *lts, int tapenum);
extern void LogicalTapeFlush(LogicalTapeSet *lts, int tapenum, BufFile *pstatefile);
extern LogicalTapeSet *LoadLogicalTapeSetState(BufFile *pfile, const char* rwfile_prefix);

extern bool LogicalTapeBackspaceTape(LogicalTapeSet *lts, LogicalTape *lt,
					 size_t size);
extern bool LogicalTapeBackspace(LogicalTapeSet *lts, int tapenum,
					 size_t size);

extern bool LogicalTapeSeekTape(LogicalTapeSet *lts, LogicalTape *lt,
				long blocknum, long offset);
extern bool LogicalTapeSeek(LogicalTapeSet *lts, int tapenum,
				long blocknum, long offset);

extern void LogicalTapeTellTape(LogicalTapeSet *lts, LogicalTape *lt,
				long *blocknum, long *offset);
extern void LogicalTapeTell(LogicalTapeSet *lts, int tapenum,
				long *blocknum, long *offset);

extern long LogicalTapeSetBlocks(LogicalTapeSet *lts);

extern void LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts);

#endif   /* LOGTAPE_H */
