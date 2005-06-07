/*-------------------------------------------------------------------------
 *	cdbtmtest.c
 *-------------------------------------------------------------------------
 */

#include <stdio.h> 
 
#include "postgres.h"
#include "cdb/cdbtmlog.h"
#include "cdb/cdbtm.h"

extern char dtm_dir[MAXPGPATH];
extern TmLogFileControlBlock tmLogFileHeader;

int
main(int argc, char  **argv)
{

    strcpy(dtm_dir,"/Users/jchang/cdb-data/cdb0/pg_dtm");
    if (initTmLog( "/Users/jchang/cdb-data/cdb0/pg_dtm"))
    {
        printf( " log is recreated" );
        return 1;
    }
    printf( "control block, CP:%d:%d\n", tmLogFileHeader.checkpointFileNo, tmLogFileHeader.checkpointOffset);
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
