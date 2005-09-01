/*-------------------------------------------------------------------------
 *	cdbtmtest.c
 *-------------------------------------------------------------------------
 */

#include <stdio.h> 
#include <dirent.h>

 
#include "postgres.h"
#include "cdb/cdbtmlog.h"
#include "cdb/cdbtm.h"

extern char dtm_dir[MAXPGPATH];
static TmLogFileControlBlock *tmLogFileHeader;
static int initTmLogx( char *dir);


int
main(int argc, char  **argv)
{

    strcpy(dtm_dir,"/Users/jchang/cdb-data/cdb0/pg_dtm");
    if (initTmLogx( "/Users/jchang/cdb-data/cdb0/pg_dtm"))
    {
        printf( " log is recreated" );
        return 1;
    }
    tmLogFileHeader = getTmLogFileHeader();
    printf( "control block, CP:%d:%d\n", tmLogFileHeader->checkpointFileNo, tmLogFileHeader->checkpointOffset);
    TmLogRecord *record1;
    record1 = readTmLogRecordNext(-1);
    while ( record1 != NULL )
    {
        printf( "%d:%d  GID:%s  RT:%d  ST:%d  NP:%d (", 
                record1->header.thisFileNo,
                record1->header.thisOffset,
                record1->header.gid,
                record1->header.recType,
                record1->header.state,
                record1->header.numberOfParticipants);
        int i=0;
        if (record1->header.numberOfParticipants != 0)
        {
            if (record1->header.recType == 0)
            {
                for (i=0; i<record1->header.numberOfParticipants; i++)
                    printf( "%d, ", record1->participants[i]);
            }
            else
            {
                for (i=0; i<record1->header.numberOfParticipants; i++)
                {
                    TMGXACT *par = (TMGXACT*)&record1->participants;
                    printf( "gid:%s,st:%d,lsn:%d:%d ", par[i].gid, par[i].state, par[i].fileno, par[i].offset);
                }   
            }
        }
        printf(")\n");
        record1 = readTmLogRecordNext(-1);
    }               
    
    
    
    
    return 0;
}

/*
 * initalize tm log manager
 */
int
initTmLogx( char *dir)
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
    }
    
    return false;
}
