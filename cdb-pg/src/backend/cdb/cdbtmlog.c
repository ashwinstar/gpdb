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
#if defined(HAVE_STRUCT_CMSGCRED) || defined(HAVE_STRUCT_FCRED) || defined(HAVE_STRUCT_SOCKCRED)
#include <sys/uio.h>
#endif

#include "cdb/cdbtmlog.h"
#include "cdb/cdblink.h"
#include "storage/fd.h"

extern int errno;

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

static bool mirrorQDInited = false;


static TmLogRecord * readTmLogRecordNext(int offset);
static void writeTmControlBlock(int offset);
static void initTmLogFile(int seqno);
static void closeTmLogFile(void);
static void removeTmLogFile(int seqno);
static bool validateLogFileControlBlock(TmLogFileControlBlock *header);
static crc64 calculateCRC(void *buf, int len);
static bool read_with_ereport(int fd, void *data, int len);
static void switchTmLogFile(void);
static void removeTmLogFileUntil(int seqno);


/*
 * read next log record. if offset is given, read from that offset.
 */
static TmLogRecord *
readTmLogRecordNext(int offset)
{
    int loffset, participant_len=0;
    TmLogRecord *record = NULL;
    TmLogRecordHeader header;
    
    elog(DEBUG1, "readTmLogRecord (%d)", offset);
    
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

/*
 * write a tm log record
 */
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

    /* sync to mirror db */
    write_to_mirror(record, record->header.thisOffset, len);
}

/*
 * write tm control block, which is located in the begining of each tm log file. 
 */
static void
writeTmControlBlock(int offset)
{
    lseek(logFileFD, 0, SEEK_SET);
    
    tmLogFileHeader.checkpointFileNo = logFileNo;
    tmLogFileHeader.checkpointOffset = offset;
    tmLogFileHeader.crc = calculateCRC( &tmLogFileHeader, 3*sizeof(uint32) ); 
    
    /* write control block */
    write_with_ereport(logFileFD, &tmLogFileHeader, sizeof(TmLogFileControlBlock));
    
    /* sync to mirror db */
    write_to_mirror(&tmLogFileHeader, 0, sizeof(TmLogFileControlBlock));
}

/*
 * write check point log record
 */
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
        memcpy(&((TMGXACT*)record->participants)[i], gxact, sizeof(TMGXACT));
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

/*
 * initialize, creating, a tm log file
 */
static void
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

/*
 * opening a tm log file
 */
bool
openTmLogFile(int seqno)
{
    char        path[MAXPATH];

	TmLogFilePath(path, seqno);

	elog(LOG, "OpenTmLogFile (%d)", seqno);
    
    // clean the log file descriptor first
    if (logFileFD >= 0)
        closeTmLogFile();
        
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

/*
 * closing a tm log file
 */
static void
closeTmLogFile(void)
{
    if ( logFileFD >= 0 )
        close(logFileFD);
    
    logFileFD=-1;
    currentOffset=-1;
    logFileNo=-1;
}

/*
 * switch to next tm log file when log file size limit is reached.
 */
static void
switchTmLogFile(void)
{
    int seqno = ++logFileNo;
    closeTmLogFile();
    
    /* create new log file */
    initTmLogFile(seqno);
    checkPointTm();
    flushTmLogFile();
}

/* flush tm log file */
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

/* remove useless tm log file */
static void
removeTmLogFileUntil(int seqno)
{
    int i=0;
    if ( seqno <= *shmEarliestLogFileNo )
        return;

    for (i=*shmEarliestLogFileNo;i<seqno;i++)
        removeTmLogFile(i);
    *shmEarliestLogFileNo = seqno; 
}

/* delete a tm log file */
static void
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

/*
 * initalize tm log manager
 */
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
    {
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open directory \"%s\": %m", dir)));
    }
   
    /* go through tm directory to find latest log file */ 
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
static bool
validateLogFileControlBlock(TmLogFileControlBlock *header)
{
    return EQ_CRC64(calculateCRC(header,3*sizeof(uint32)), header->crc);
}

static crc64
calculateCRC(void *buf, int len)
{
    crc64 calc_crc;
    
    INIT_CRC64(calc_crc);
    COMP_CRC64(calc_crc, buf, len);
    FIN_CRC64(calc_crc);

    return calc_crc;
}

static bool
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

void
write_to_mirror(void *buf, int offset, int len)
{
    char cmd[40];
    
    /* initialize mirror qd sync */
    if (!mirrorQDInited)
    {
        mirrorQDInited = true;
        write_to_mirror(NULL, offset, 0);
    }
    
    sprintf(cmd, "tmlog %08x %d %d", logFileNo, offset, len);
    write_qd_sync(cmd, buf, len); 
}

bool
resync_tmlog_to_mirror(int startoff, int stopoff)
{
    char buf[8000], cmd[50];
    int len, loffset;
    len = stopoff - startoff;
    loffset = lseek(logFileFD, startoff, SEEK_SET);
    Assert(loffset == startoff);
    elog(LOG, "resyncing start offset: %d, stop offset: %d", startoff, stopoff);
    while (len != 0)
    {
        int read_len = sizeof(buf);
        if (len < sizeof(buf))
            read_len = len; 
        if (!read_with_ereport(logFileFD, buf, read_len))
        {
            elog(LOG, "failed to resync tmlog at offset: %d", loffset);
            return false;
        }

        /* write to symc agent */
        sprintf(cmd, "tmlog %08x %d %d resync", logFileNo, loffset, read_len);
        write_qd_sync(cmd, buf, read_len); 
        
        loffset += read_len;
        len -= read_len;
    }

    /* successful */
    return true;
}

