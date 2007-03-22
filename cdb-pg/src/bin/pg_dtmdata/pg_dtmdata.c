/*-------------------------------------------------------------------------
 *	pg_dtmdata.c
 *		Utility to read and display the data for latest dtm log file.
 * 
 * Copyright (c) 2005-2006, greenplum inc
 *
 * $Id$
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>


#define PROVIDE_64BIT_CRC
#include "postgres.h"
#include "cdb/cdbtmlog.h"
#include "cdb/cdbtm.h"



static char * getLatestLogFile(const char *progname, const char *DataDir);
static char * getRecTypeAsString( int recType );
static char * getStateAsString( int state );
static TmLogRecordHeader * readNextHeader(const char *progname, int fd, TmLogRecordHeader *hdr );



static TmLogFileControlBlock dtmLogFileHeader = {TM_FILE_MAGIC, 0, 0, {0}};




static void
usage( const char *progname)
{
	printf(_("%s displays dtm log information of a Greenplum Database instance.\n\n"), progname);
	
	printf(_("Usage:\n"
			 "  %s [OPTION] [DATADIR]\n\n"
			 "Options:\n"
			 "  --help  		show this help, then exit\n"
			 "  --version		output version information, then exit\n"), progname);
	
	printf(_("\nIf no data directory (DATADIR) is specified, "
			 "the environment variable PGDATA\nis used.\n\n"));
}



int
main(int argc, char *argv[])
{

	char	*DtmLogFilePath;

	char *DataDir;
	const char *progname;
	int i, fd;
	TmLogRecordHeader *header;
	
	set_pglocale_pgservice(argv[0], "pg_dtmdata");
	progname = get_progname(argv[0]);

	/* we'll reuse a single header since its a fixed size */
	header = malloc(sizeof(TmLogRecordHeader));

	/* 
	 * Process command line arguments 
	 */
	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dtmdata (Greenplum Database) " PG_VERSION);
			exit(0);
		}
	}

	if (argc > 1)
		DataDir = argv[1];
	else
		DataDir = getenv("PGDATA");
	if (DataDir == NULL)
	{
		fprintf(stderr, _("%s: no data directory specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}
	
	
	/* 
	 * Figure out which is the latest dtm log file
	 */
	DtmLogFilePath = getLatestLogFile(progname,DataDir);
	
	
	/*
	 * Open the file 
	 */
	if ((fd = open(DtmLogFilePath, O_RDONLY)) == -1)
	{
		fprintf(stderr, _("%s: could not open file \"%s\" for reading: %s\n"),
				progname, DtmLogFilePath, strerror(errno));
		exit(2);
	}
	
	
	/*
	 * Read and display the log file header
	 */ 
	if (read(fd, &dtmLogFileHeader, sizeof(TmLogFileControlBlock)) != sizeof(TmLogFileControlBlock))
	{
		fprintf(stderr, _("%s: could not read file \"%s\": %s\n"),
				progname, DtmLogFilePath, strerror(errno));
		exit(2);
	}

	printf(_("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"));
	
	printf(_("DTM Log control block:\n\tmagic: %d\n\tLast Checkpoint: (fileNo: %d Offset: %d) \n"),
			dtmLogFileHeader.magic, dtmLogFileHeader.checkpointFileNo, dtmLogFileHeader.checkpointOffset);
	
	printf(_("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"));
	/*
	 * Read and display all of the entries in the file
	 */
	while( (header = readNextHeader(progname, fd, header)) != NULL ) 
	{
		
		/* read header to figure out which of the 2 types of log entries
		 * we are dealing with.
		 */
		printf(_("RECORD:%d/%d  GID:%s  RecType:%s  state:%s  numParticipants:%d \n"),
			     header->thisFileNo,
			     header->thisOffset,
			     header->gid,
			     getRecTypeAsString(header->recType),
			     getStateAsString(header->state),
			     header->numberOfParticipants);
			     
		if (header->recType == TM_LOG_XACT )
		{
			/* read the body of the dtx entry record */
			int dataToRead = sizeof(int) * header->numberOfParticipants;
			
			if( dataToRead > 0 )
			{	
				int *parts = malloc(dataToRead);
			
				int len = read(fd, parts, dataToRead);
				
				if (len != dataToRead)
				{
					fprintf(stderr, _("\tCorrupt DTXN body: %s\n)"), strerror(errno));
					exit(2); 				
				}	
				
				
				printf(_("\t"));
				for (i=0; i < header->numberOfParticipants; i++)
				{
					printf("%d, ", parts[i] );
				}
				printf(_("\n"));
				
				
				free(parts);			
			}
		}
		else if (header->recType == TM_LOG_CHECKPOINT)
		{
			/* read the body of the checkpoint record */
			int dataToRead = sizeof(TMGXACT_LOG) * header->numberOfParticipants;
			
			if (dataToRead > 0)
			{
				TMGXACT_LOG *xacts = malloc(dataToRead);
				
				int len = read(fd, xacts, dataToRead);
				
				if (len != dataToRead)
				{
					fprintf(stderr, _("\tCorrupt CHECKPOINT body: %s\n)"), strerror(errno));
					exit(2);
				}
				
				printf(_("\n\tCHECKPOINT BODY:"));
				for (i=0; i < header->numberOfParticipants; i++)
				{
					printf(_("\tGID:%s state:%s lsn:%d:%d \n"), xacts[i].gid, getStateAsString(xacts[i].state), 
							xacts[i].fileno, xacts[i].offset);	
				}
				printf(_("\n"));
			}
		}
		
	}
	
	/*mojo*/
	return 0;
}



/* getRecTypeAsString
 *
 * displays the dtm log record type as a string 
 */
char *
getRecTypeAsString( int recType )
{
	
	switch(recType)
	{
		case TM_LOG_XACT:
			return "DTX ENTRY";
		case TM_LOG_CHECKPOINT:
			return "CHECKPOINT";
		default:
			return "INVALID";
	}
}


/* getStateAsString
 *
 * displays the dtx state as a string 
 */
char *
getStateAsString( int state )
{
	switch(state)
	{
		case DTX_STATE_IN_PROGRESS: 
			return "IN PROGRESS";
		case DTX_STATE_PREPARED:
			return "PREPARED";
		case DTX_STATE_1P_COMMIT:		/* 1p commit */
			return "1P COMMIT";			
		case DTX_STATE_2P_COMMIT:		/* 2p commit */
			return "2P COMMIT";
		case DTX_STATE_ROLLBACK:			/* rollback */
			return "ROLLBACK";
		case DTX_STATE_ABORT:		/* abort */
			return "ABORT";
		case DTX_STATE_COMMIT_DONE:		/* commit done */
			return "COMMIT DONE";
		case DTX_STATE_ROLLBACK_DONE:		/* rollback done */
			return "ROLLBACK DONE";
		default:
			return "INVALID";

	}
}



/* readNextHeader
 *
 * attempts to read in the next record header.  The header is read into the 
 * hdr pointer passed in.  (it is also returned back).
 */
TmLogRecordHeader *
readNextHeader(const char *progname, int fd, TmLogRecordHeader *hdr )
{
	int len = read(fd, hdr, sizeof(TmLogRecordHeader));
	
	if (len != sizeof(TmLogRecordHeader))
	{
		if (errno == 0)
		{
			return NULL;
		}
		else
		{
			fprintf(stderr, _("%s: could not read header of log record: %s\n"),
				progname, strerror(errno));
			exit(2); 
		}
	}	
	
	return hdr;
}


/* getLatestLogfile
 *
 * used to locate the latest DTM log file and return a string with the 
 * fully qualified name of the file.
 * 
 * - DataDir is the path of the pg data directory.
 */ 
static
char *
getLatestLogFile(const char *progname, const char *DataDir)
{
	DIR	*dtmDir;
	struct dirent *dirEntry;
	int id = 0;
	int file = -1;
	char dtmDirPath[MAXPGPATH];
	char *fileName = malloc(sizeof(char) * MAXPGPATH);
	
	snprintf(dtmDirPath, MAXPGPATH, "%s/pg_dtm/", DataDir);
	
	dtmDir = opendir(dtmDirPath);
	
	if (dtmDir == NULL)
	{
		fprintf(stderr, _("%s: could not open directory \"%s\" for reading: %s\n"),
				progname, DataDir, strerror(errno));
		exit(2);
	}
	
	/* look at all the files in the dir to find the latest one */
	while ((dirEntry = readdir(dtmDir)) != NULL)
	{
		if( *dirEntry->d_name == '.')
			continue;
			
			sscanf(dirEntry->d_name, "%d", &id);
			
			if (id > file)
				file = id;
	}
	
	closedir(dtmDir);
	
	if( file == -1)
	{
		fprintf(stderr, _("%s: could not find any dtm logfiles in directory"
						  "\"%s\" for reading: %s\n"),
				progname, DataDir, strerror(errno));
		exit(2);
	}
	
	
	/* TODO: unify this file naming convention with cdbtmlog.c */
	snprintf(fileName, MAXPGPATH, "%s/%08X", dtmDirPath, file);	
	
	return fileName;
}



