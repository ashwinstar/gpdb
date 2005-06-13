/*-------------------------------------------------------------------------
 *	cdbtmlog.c
 *-------------------------------------------------------------------------
 */
 
#include "postgres.h"

#include <fcntl.h>
#include <dirent.h>
//#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "cdb/cdbtmlog.h"
#include "storage/fd.h"

extern int errno;
extern char dtm_dir[MAXPGPATH];

/*
 * Dir is set during TmShmemInit and does not change thereafter.
 * The value is automatically inherited by backends via fork, and
 * doesn't need to be in shared memory.
 */

#define MAXPATH  1024
#define MAXLOGFILESIZE 50000000    /* 50M */

/************************************************************************/
/* Shared memory bookkeeping                                            */
/************************************************************************/

#define TmLogFilePath(path, seqno) \
	snprintf(path, MAXPGPATH, "%s/%08X", dtm_dir, seqno)


#define TM_FILE_MAGIC   823487293
static TmLogFileControlBlock tmLogFileHeader={TM_FILE_MAGIC,0,0,{0}};
static int  currentOffset=-1;
static int  logFileFD=-1, logFileNo;

TmLogRecord *
readTmLogRecordNext(int offset)
{
    int loffset, participant_len=0;
    TmLogRecord *record = NULL;
    
    elog(DEBUG1, "readTmLogRecord (%d)", offset);
    TmLogRecordHeader header;
    
    if (offset == -1)
        offset = currentOffset;
    
    loffset = lseek(logFileFD, offset, SEEK_SET);
    Assert(loffset == offset);

    /* Read record header */
    if (!read_with_ereport(logFileFD, &header, sizeof(TmLogRecordHeader)))
    {
        /* could be the last corrupted record, ignore it. */
        return NULL;
    }

    Assert(header.thisOffset == offset);
    
    if (header.numberOfParticipants == 0)
    {
        record = malloc(sizeof(TmLogRecordHeader));
        memcpy(record, &header, sizeof(TmLogRecordHeader));
    }
    else
    {
        switch (header.recType)
        {
            case TM_LOG_XACT:
                participant_len = sizeof(int) * header.numberOfParticipants;
                break;
            case TM_LOG_CHECKPOINT:
                participant_len = sizeof(TMGXACT) * header.numberOfParticipants;
                break;
        }

        record = malloc(sizeof(TmLogRecordHeader) + participant_len);
        memcpy(record, &header, sizeof(TmLogRecordHeader));
        /* Read Participants */
        if (!read_with_ereport(logFileFD, &record->participants, participant_len))
        {
            /* could be the last corrupted record, ignore it */
            return NULL;
        }
    }
    
    currentOffset = offset + sizeof(TmLogRecordHeader) + participant_len;
    return record;
}

void
writeTmLogRecord(TmLogRecord *record)
{
    int len;
    
    if ( logFileNo != *shmLatestLogFileNo )
    {
        /* log file is incremented */
        closeTmLogFile();
        openTmLogFile( *shmLatestLogFileNo );
    }

    if ( lseek(logFileFD, 0, SEEK_END) > MAXLOGFILESIZE )
    {
        /* switch log file */
        switchTmLogFile();
    }

    record->header.thisFileNo = logFileNo;
    record->header.thisOffset = lseek(logFileFD, 0, SEEK_END);
    Assert(record->header.thisOffset != -1);
    
    len=sizeof(TmLogRecordHeader);
    if (record->header.numberOfParticipants != 0)
    {
        switch (record->header.recType)
        {
            case TM_LOG_XACT:
                len += sizeof(int) * record->header.numberOfParticipants;
                break;
            case TM_LOG_CHECKPOINT:
                len += sizeof(TMGXACT) * record->header.numberOfParticipants;
                break;
        }
    }
    
    /* Read record header */
    write_with_ereport(logFileFD, record, len);
}

void
writeTmControlBlock(int offset)
{
    lseek(logFileFD, 0, SEEK_SET);
    
    tmLogFileHeader.checkpointFileNo = logFileNo;
    tmLogFileHeader.checkpointOffset = offset;
    tmLogFileHeader.crc = calculateCRC( &tmLogFileHeader, 3*sizeof(uint32) ); 
    
    /* write control block */
    write_with_ereport(logFileFD, &tmLogFileHeader, sizeof(TmLogFileControlBlock));
}

void
checkPointTm(void)
{   
    TMGXACT *gxact;
    int n = startTMXactScan();
    int earlierFileNo=-1;
    TmLogRecord *record=NULL;
    int i=0;
    
    record = palloc(sizeof(TmLogRecordHeader) + n*sizeof(TMGXACT));

    for ( i=0;i<n;i++ )
    {
        gxact = getNextTMXact(i);
        memcpy(&record->participants[i], gxact, sizeof(TMGXACT));
        if ( earlierFileNo == -1 || gxact->fileno < earlierFileNo )
            earlierFileNo = gxact->fileno;
    }
    endTMXactScan();
    
    strcpy(record->header.gid, "CHECKPOINT");
    record->header.recType = TM_LOG_CHECKPOINT;
    record->header.state = 0;
    record->header.numberOfParticipants = n;


    /* write check point log record */
    writeTmLogRecord(record);
    
    //write log control header
    writeTmControlBlock(record->header.thisOffset);

    /* deallocate record */
    pfree(record);
    
    /* flush tm log file */
    flushTmLogFile();
    
    /* remove old log file */
    removeTmLogFileUntil( earlierFileNo );
}

void
initTmLogFile(int seqno)
{
    char        path[MAXPATH];

    TmLogFilePath(path, seqno);

    elog(LOG, "InitTmLogFile (%d)", seqno);

    logFileFD = open(path, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (logFileFD < 0)
    {
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not create tm log file \"%s\"", 
                        path)));
    }

    *shmLatestLogFileNo = logFileNo = seqno;
    writeTmControlBlock(0);
    currentOffset = sizeof(TmLogFileControlBlock);
}

bool
openTmLogFile(int seqno)
{
    char        path[MAXPATH];

	TmLogFilePath(path, seqno);

	elog(LOG, "OpenTmLogFile (%d)", seqno);

    logFileFD = open(path, O_RDWR, 0);
	if (logFileFD < 0)
	{
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open tm log file \"%s\", file can not be opened", 
                        path)));
	}

    logFileNo = seqno;
    if (!read_with_ereport(logFileFD, &tmLogFileHeader, sizeof(TmLogFileControlBlock)))
    {
        /* partial control block, file is not created properly */
        return false;
    }
    currentOffset = sizeof(TmLogFileControlBlock);

    if (!validateLogFileControlBlock(&tmLogFileHeader)
        || tmLogFileHeader.magic != TM_FILE_MAGIC)
    {
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open tm log file \"%s\", file control block corrupted", 
                        path)));
    }
    
    /* if file is not created completely */
    if (tmLogFileHeader.checkpointOffset == 0)
        return false;
    
    return true;
}

void
closeTmLogFile(void)
{
    if ( logFileFD >= 0 )
        close(logFileFD);
    
    logFileFD=-1;
    currentOffset=-1;
    logFileNo=-1;
}

void
switchTmLogFile(void)
{
    int seqno = ++logFileNo;
    closeTmLogFile();
    
    /* create new log file */
    initTmLogFile(seqno);
    checkPointTm();
    flushTmLogFile();
}

void
flushTmLogFile(void)
{
    int ret = fsync(logFileFD);
    if (ret != 0)
    {
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not fsync tm log file \"%d\": %m",
                        logFileNo)));
    }
}

void
removeTmLogFileUntil(int seqno)
{
    int i=0;
    if ( seqno <= *shmEarliestLogFileNo )
        return;

    for (i=*shmEarliestLogFileNo;i<seqno;i++)
        removeTmLogFile(i);
    *shmEarliestLogFileNo = seqno; 
}

void
removeTmLogFile(int seqno)
{
	char path[MAXPGPATH];
	TmLogFilePath(path, seqno);

	if(unlink(path))
    {
        if (errno == ENOENT)
            return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove tm log file \"%s\"", 
						path)));
    }
}

int
initTmLog( char *dir)
{
    DIR                *cldir;
    struct dirent *clde;
    /* sort files, should have only two files at most */
    int file1=-1,    /* lastest file */
        file2=-1,    /* second lastest file */
        id=0;

    elog(DEBUG1, "Discovering tm log files");
    
    cldir = opendir(dir);
    if (cldir == NULL)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open directory \"%s\": %m", dir)));
    
    while ((clde = readdir(cldir)) != NULL)
    {
        if (*clde->d_name == '.')
            continue;
        
        sscanf(clde->d_name, "%d", &id);
        if ( id > file1 )
        {
            file2 = file1;
            file1 = id;
        }
        else if ( id > file2 )
            file2 = id;
        if ( *shmEarliestLogFileNo == -1 || id < *shmEarliestLogFileNo )
            *shmEarliestLogFileNo = id;
    }
    closedir(cldir);
    
    if (file1 == -1)
    {
        /* no file found, create the first log file */
        initTmLogFile(0);
        checkPointTm();
        return true;
    }
    else
    {
        if ( !openTmLogFile(file1) )
        {
            if ( file2 == -1 || !openTmLogFile(file2) )
            {
                /* cleanup */
                removeTmLogFile(file2);
                /* no valid tm log file found, recreate */
                initTmLogFile(0);
                checkPointTm();
                return true;
            }
            removeTmLogFile(file1);
            *shmLatestLogFileNo = file2;
        }
        else
            *shmLatestLogFileNo = file1;
    }
    
    return false;
}

/*
 * Scans the pg_twophase directory and recovers every found 
 * prepared transaction
 */
void
recoverTmLog(void)
{
    int i=0;
    TmLogRecord *record=NULL;
	elog(LOG, "Recovering TM");

    if ( initTmLog(dtm_dir) )
        /* tm log is new initialized, no recovery needed */
        return;
        
    /* recovery */
    
    /* read checkpoint log record */
    Assert(tmLogFileHeader.checkpointFileNo == logFileNo);
    record = readTmLogRecordNext( tmLogFileHeader.checkpointOffset );
    if (record == NULL)
    {
        /* checkpoint failure */
        ereport(ERROR,
            (errcode_for_file_access(),
             errmsg("tm log file is corrupted \"%.08d\"", logFileNo)));
    }
    Assert( record->header.recType == TM_LOG_CHECKPOINT );

    for ( i=0;i<record->header.numberOfParticipants;i++ )
    {
        restoreGxact(&((TMGXACT*)record->participants)[i]);
    }
    free(record);
    
    /* read subsequent log record */
    while ( (record = readTmLogRecordNext(-1)) != NULL )
    {
        if (record->header.recType != TM_LOG_CHECKPOINT)
            redoTmLog(record);
        free(record);
    }
}

/*
 * Validates the log file header. Returns true if
 * it looks OK.
 */
bool
validateLogFileControlBlock(TmLogFileControlBlock *header)
{
    return EQ_CRC64(calculateCRC(header,3*sizeof(uint32)), header->crc);
}

crc64
calculateCRC(void *buf, int len)
{
    crc64 calc_crc;
    
    INIT_CRC64(calc_crc);
    COMP_CRC64(calc_crc, buf, len);
    FIN_CRC64(calc_crc);

    return calc_crc;
}

bool
read_with_ereport(int fd, void *data, int len)
{
    int read_len;

    errno = 0;
    read_len = read(fd, data, len);
    
    if (read_len == 0)
        return false;
    else if (read_len != len && errno == 0)
        return false;
    else if (errno != 0)
    {
        close(fd);
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("error reading tm log file: %m")));
    }
    
    return true;
}

void
write_with_ereport(int fd, void *data, int len)
{
    int write_len;

    write_len = write(fd, data, len);

    if(write_len != len)
    {
        close(fd);
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("error writing tm log file: %m")));
    }
}

