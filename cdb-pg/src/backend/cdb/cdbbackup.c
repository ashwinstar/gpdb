/*-------------------------------------------------------------------------
 *
 * cdbbackup.c
 *
 *
 *
 */
#include "postgres.h"
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <regex.h>
#include "libpq-fe.h"
#include "libpq/libpq-be.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbbackup.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"

/* general utility copied from cdblink.c */
#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

/* static helper functions */
static bool createBackupDirectory( char* pszPathName );
static char* findAcceptableBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey, int segid );
static char* formBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey );
static char* formStatusFilePathName( char* pszBackupDirectory, char* pszBackupKey, bool bIsBackup );
static char* relativeToAbsolute( char* pszRelativeDirectory );
static void testCmdsExist( char* pszCompressionProgram, char* pszBackupProgram );
static bool testProgramExists(char* pszProgramName);
static void validateBackupDirectory( char* pszBackupDirectory );

typedef enum backup_file_type
{
	BFT_BACKUP = 0,
	BFT_BACKUP_STATUS = 1,
	BFT_RESTORE_STATUS = 2
} BackupFileType;

/*
 * cdb_backup_launch__( TEXT, TEXT, TEXT, TEXT, TEXT ) returns TEXT
 *
 * Called by cdb_dump.c to launch cdb_dump_agent on this host.
 */
PG_FUNCTION_INFO_V1(cdb_backup_launch__);
Datum
cdb_backup_launch__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list.	*/
	char* pszBackupDirectory = "./";
	char* pszBackupKey = "";
	char* pszCompressionProgram  = "";
	char* pszPassThroughParameters = "";
	char* pszPassThroughCredentials = "";
    char* pszDataOption = "";
	
	if ( !PG_ARGISNULL(0) )
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if ( *pszBackupDirectory == '\0' )
			pszBackupDirectory = "./";
	}
	
	if ( !PG_ARGISNULL(1) )
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));
	
	if ( !PG_ARGISNULL(2) )
		pszCompressionProgram = GET_STR(PG_GETARG_TEXT_P(2));

	if ( !PG_ARGISNULL(3) )
		pszPassThroughParameters = GET_STR(PG_GETARG_TEXT_P(3));
	
	if ( !PG_ARGISNULL(4) )
		pszPassThroughCredentials = GET_STR(PG_GETARG_TEXT_P(4));
	
	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute( pszBackupDirectory );
	
	/*
	 * See whether the directory exists.  
	 * If not, try to create it.  
	 * If so, make sure it's a directory
	 */
	 validateBackupDirectory( pszBackupDirectory ); 
	
	/* Form backup file path name */
	char* pszBackupFileName = formBackupFilePathName( pszBackupDirectory, pszBackupKey );
    char* pszStatusFileName = formStatusFilePathName( pszBackupDirectory, pszBackupKey, true );
	/* Make sure we can create this file before we spin off sh
	 * cause we don't get a good error message from sh if we can't write to the file
	 */
	FILE* ftest = fopen( pszBackupFileName, "w" );
	if ( ftest == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error creating file %s in cdb_backup_launch", 
				 pszBackupFileName)));
	}
	fclose( ftest );
	
	/* Validate existence of cdb_dump_agent and compression program, if possible */
	testCmdsExist( pszCompressionProgram, "cdb_dump_agent" );
	
	char* pszDBName = NULL;
	char* pszUserName = NULL;
	if ( MyProcPort != NULL )
	{
		pszDBName = MyProcPort->database_name;
		pszUserName = MyProcPort->user_name;
	}
	
	if ( pszDBName == NULL )
		pszDBName = "";
	if ( pszUserName == NULL )
		pszUserName = "";
	
	// Fork off cdb_dump_agent 
	pid_t newpid = fork();
	if ( newpid < 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Could not fork a process for backup of database %s", pszDBName)));
	}
	else if ( newpid == 0 )
	{
		/* Child Process */
		char* pszCmdLine;
		
		
		int port = PostPortNumber;
	
		/* Create the --cdb-k parameter string */
		char* pszKeyParm = (char *)palloc( strlen(pszBackupKey) + 
											strlen(pszPassThroughCredentials) + 
											3 + 10 + 10 + 1  );
		if ( pszKeyParm == NULL )
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
 						
        getCdbCdb();			
		int instid	= (Cdb_segment==-1)?1:0; /*dispatch node*/	
		int segid	= CdbIdentity.dbid;
        /* dump schema only for entry db, and data only for segment db */
         pszDataOption = (instid==1)?"-s":"-a";									
		sprintf( pszKeyParm, "%s_%d_%d_%s", pszBackupKey, instid, segid, pszPassThroughCredentials );

		/* In the following calc for the number of characters in the pszCmdLine,  
		 * I show each piece as the count of the string for that piece  + 1 for the space
		 * after that piece, e.g. -p comes out 2 for the -p and 1 for the space
		 * after the -p  The last 1 is for the binary zero at the end.
		 */
		int len = 14 + 1  /* cdb_dump_agent */
				+ 7 + 1  /* --cdb-k */
				+ strlen(pszKeyParm) + 1
				+ 7 + 1  /* --cdb-d */
				+ strlen(pszBackupDirectory) + 1
				+ 2 + 1  /* -p */
				+ 5 + 1  /* port */
				+ 2 + 1  /* -U */
				+ strlen(pszUserName) + 1   /* pszUserName */
				+ strlen(pszPassThroughParameters) + 1
                 + 2 + 1 /* -s or -a */
				+ strlen(pszDBName) + 1
				+ 1 + 1		/* > */
				+ strlen(pszBackupFileName)
                 + 2 + 1     /* 2> */
                 + strlen(pszStatusFileName)
				+ 1;
		
		if ( pszCompressionProgram[0] != '\0' )
		{
			len += 1 + 1		/* | */
					+ strlen(pszCompressionProgram) + 1;
						
			pszCmdLine = (char *) palloc( len );
			if ( pszCmdLine == NULL )
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
			
			sprintf( pszCmdLine, "cdb_dump_agent --cdb-k %s --cdb-d %s -p %d -U %s %s %s %s | %s > %s 2> %s", 
					pszKeyParm, pszBackupDirectory, port, pszUserName, pszPassThroughParameters, pszDataOption, pszDBName, 
					pszCompressionProgram, pszBackupFileName, pszStatusFileName );
		}
		else
		{
			pszCmdLine = (char *) palloc( len );
			if ( pszCmdLine == NULL )
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
			
			sprintf( pszCmdLine, "cdb_dump_agent --cdb-k %s --cdb-d %s -p %d -U %s %s %s %s > %s 2> %s", 
					pszKeyParm, pszBackupDirectory, port, pszUserName, pszPassThroughParameters, pszDataOption, pszDBName, 
					pszBackupFileName, pszStatusFileName );
		}

        elog(DEBUG1, "cdb_dump_agent command line: %s", pszCmdLine),
			
		/* This execs a shell that runs the cdb_dump_agent program  */	
		execl("/bin/sh", "sh", "-c", pszCmdLine, NULL );

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error in cdb_backup_launch - execl of %s with Command Line %s failed", 
				 "/bin/sh", pszCmdLine)));
		_exit(127);		
	}
	
	assert( pszBackupFileName != NULL && pszBackupFileName[0] != '\0' );
	int len_name = strlen(pszBackupFileName);
	int len = len_name + VARHDRSZ;
	text* result = (text *) palloc(len);
	if ( result == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, pszBackupFileName, len_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * cdb_restore_launch__( TEXT, TEXT, TEXT, TEXT, TEXT, bool ) returns TEXT
 *
 * Called by cdb_restore.c to launch cdb_restore_agent on this host.
 */
PG_FUNCTION_INFO_V1(cdb_restore_launch__);
Datum
cdb_restore_launch__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list.	*/
	char*	pszBackupDirectory = "./";
	char*	pszBackupKey = "";
	char*	pszCompressionProgram  = "";
	char*	pszPassThroughParameters = "";
	char*	pszPassThroughCredentials = "";
	bool	bOnErrorStop = false;
	
    getCdbCdb();
    int instid  = (Cdb_segment==-1)?1:0; /*dispatch node*/ 
    int segid   = CdbIdentity.dbid;                                     
											
	if ( !PG_ARGISNULL(0) )
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if ( *pszBackupDirectory == '\0' )
			pszBackupDirectory = "./";
	}
	
	if ( !PG_ARGISNULL(1) )
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));
	
	if ( !PG_ARGISNULL(2) )
		pszCompressionProgram = GET_STR(PG_GETARG_TEXT_P(2));

	if ( !PG_ARGISNULL(3) )
		pszPassThroughParameters = GET_STR(PG_GETARG_TEXT_P(3));
	
	if ( !PG_ARGISNULL(4) )
		pszPassThroughCredentials = GET_STR(PG_GETARG_TEXT_P(4));
	
	if ( !PG_ARGISNULL(5) )
		bOnErrorStop = PG_GETARG_BOOL(5);
	
	char* pszOnErrorStop = bOnErrorStop ? "--cdb-e" : ""; 
		
	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute( pszBackupDirectory );
	
	/* Form backup file path name */
	char* pszBackupFileName = formBackupFilePathName( pszBackupDirectory, pszBackupKey );
    char* pszStatusFileName = formStatusFilePathName( pszBackupDirectory, pszBackupKey, false );
    
	/* Make sure pszBackupFileName exists */
	struct stat info;
	if ( 0 != stat( pszBackupFileName, &info ) )
	{
		pszBackupFileName = findAcceptableBackupFilePathName( pszBackupDirectory, pszBackupKey, segid );
		if ( pszBackupFileName == NULL )
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("No Backup FileName could be found to restore segment %d from directory %s with backup key %s", segid, pszBackupDirectory, pszBackupKey )));
	}
	
	/* Validate existence of compression program and cdb_restore_agent program */
	testCmdsExist( pszCompressionProgram, "cdb_restore_agent" );
	
	char* pszDBName = NULL;
	char* pszUserName = NULL;
	if ( MyProcPort != NULL )
	{
		pszDBName = MyProcPort->database_name;
		pszUserName = MyProcPort->user_name;
	}
	
	if ( pszDBName == NULL )
		pszDBName = "";
	if ( pszUserName == NULL )
		pszUserName = "";
	
	// Fork off cdb_restore_agent 
	pid_t newpid = fork();
	if ( newpid < 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Could not fork a process for backup of database %s", pszDBName)));
	}
	else if ( newpid == 0 )
	{
		/* Child Process */
		char* pszCmdLine;
		
		int port = PostPortNumber;
	
		/* Create the --cdb-k parameter string */
		char* pszKeyParm = (char *)palloc( strlen(pszBackupKey) + 
											strlen(pszPassThroughCredentials) + 
											3 + 10 + 10 + 1 );
		if ( pszKeyParm == NULL )
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
 									
		sprintf( pszKeyParm, "%s_%d_%d_%s", pszBackupKey, instid, segid, pszPassThroughCredentials );
		
		/* In the following calc for the number of characters in the pszCmdLine,  
		* I show each piece as the count of the string for that piece  + 1 for the space
		* after that piece, e.g. -p comes out 2 for the -p and 1 for the space
		* after the -p  The last 1 is for the binary zero at the end.
		*/
		int len = 17 + 1  /* cdb_restore_agent */
				+ 7 + 1  /* --cdb-k */
				+ strlen(pszKeyParm) + 1
				+ 7 + 1  /* --cdb-d */
				+ strlen(pszBackupDirectory) + 1
				+ 7 + 1  /* --cdb-e */
				+ 2 + 1  /* -p */
				+ 5 + 1  /* port */
				+ 2 + 1  /* -U */
				+ strlen(pszUserName) + 1   /* pszUserName */
				+ strlen(pszPassThroughParameters) + 1
				+ 2 + 1  /* -d */
				+ strlen(pszDBName) + 1
                 + 1 + 1  /* > */
                 + strlen(pszStatusFileName)
                 + 4 + 1  /* 2>&1 */
                 + 1;
		
		if ( pszCompressionProgram[0] != '\0' )
		{
			len += 4 + strlen(pszCompressionProgram) + 
					+ strlen(pszBackupFileName); 
						
			pszCmdLine = (char *) palloc( len );
			if ( pszCmdLine == NULL )
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("out of memory")));
			}
			
			sprintf( pszCmdLine, "%s %s | cdb_restore_agent --cdb-k %s --cdb-d %s %s -p %d -U %s %s -d %s > %s 2>&1", 
					pszCompressionProgram, pszBackupFileName, pszKeyParm, pszBackupDirectory, pszOnErrorStop, port, pszUserName, 
					pszPassThroughParameters, pszDBName, pszStatusFileName );
		}
		else
		{
			len += strlen(pszBackupFileName) + 1;

			pszCmdLine = (char *) palloc( len );
			if ( pszCmdLine == NULL )
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("out of memory")));
			}
			
			sprintf( pszCmdLine, "cdb_restore_agent --cdb-k %s --cdb-d %s %s -p %d -U %s %s -d %s %s > %s 2>&1", 
					pszKeyParm, pszBackupDirectory, pszOnErrorStop, port, pszUserName, pszPassThroughParameters, pszDBName, 
					pszBackupFileName, pszStatusFileName );
		}

			
		elog(DEBUG1, "cdb_restore_agent command line: %s", pszCmdLine),
		
		/* This execs a shell that runs the cdb_restore_agent program  */	
		execl("/bin/sh", "sh", "-c", pszCmdLine, NULL );

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error in cdb_restore_launch - execl of %s with Command Line %s failed", 
				 "/bin/sh", pszCmdLine)));
		_exit(127);		
	}
	
	assert( pszBackupFileName != NULL && pszBackupFileName[0] != '\0' );
	
	int len_name = strlen(pszBackupFileName);
	int len = len_name + VARHDRSZ;
	text* result = (text *) palloc(len);
	if ( result == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, pszBackupFileName, len_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * cdb_read_backup_file__( TEXT, TEXT, int ) returns TEXT
 *
 */
PG_FUNCTION_INFO_V1(cdb_read_backup_file__);
Datum
cdb_read_backup_file__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list.	*/
	char*	pszBackupDirectory = "./";
	char*	pszBackupKey = "";
	int		fileType = 0;
	
	if ( !PG_ARGISNULL(0) )
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if ( *pszBackupDirectory == '\0' )
			pszBackupDirectory = "./";
	}
	
	if ( !PG_ARGISNULL(1) )
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));
	
	if ( !PG_ARGISNULL(2) )
		fileType = PG_GETARG_INT32(2);
	
	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute( pszBackupDirectory );
	
	/* Form backup file path name */
	char* pszFileName = NULL;
	switch ( fileType )
	{
		case BFT_BACKUP:
			pszFileName = formBackupFilePathName( pszBackupDirectory, pszBackupKey );
			break;
		case BFT_BACKUP_STATUS:
		case BFT_RESTORE_STATUS:
			pszFileName = formStatusFilePathName( pszBackupDirectory, pszBackupKey, (fileType == BFT_BACKUP_STATUS) );
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					errmsg("Invalid filetype %d passed to cdb_read_backup_file", fileType )));
			break;
	}
	
	/* Make sure pszFileName exists */
	struct stat info;
	if ( 0 != stat( pszFileName, &info ) )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Backup File %s Type %d could not be be found", pszFileName, fileType )));
	}
	
	/* Read file */
	if ( info.st_size > INT_MAX )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Backup File %s Type %d too large to read", pszFileName, fileType )));
	}
	
	/* Allocate enough memory for entire file, and read it in. */
	char* pszBackup = (char *)palloc( info.st_size + 1 );
	if ( pszBackup == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	FILE* f = fopen( pszFileName, "r" );
	if ( f == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Backup File %s Type %d cannot be opened", pszFileName, fileType )));
	}
	
	if ( info.st_size != fread( pszBackup, 1, info.st_size, f ) )
	{
		fclose( f );
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Error reading Backup File %s Type %d", pszFileName, fileType )));
	}
	
	fclose( f );
	f = NULL;
	pszBackup[info.st_size ] = '\0';
	
	int len_status = strlen(pszBackup);
	int len = len_status + VARHDRSZ;
	text* result = (text *) palloc(len);
	if ( result == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, pszBackup, len_status);

	PG_RETURN_TEXT_P(result);
}

/*
 * cdb_write_backup_file__( TEXT, TEXT, TEXT ) returns TEXT
 *
 */
PG_FUNCTION_INFO_V1(cdb_write_backup_file__);
Datum
cdb_write_backup_file__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list.	*/
	char*	pszBackupDirectory = "./";
	char*	pszBackupKey = "";
	char*	pszBackup = "";
	
	if ( !PG_ARGISNULL(0) )
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if ( *pszBackupDirectory == '\0' )
			pszBackupDirectory = "./";
	}
	
	if ( !PG_ARGISNULL(1) )
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));
	
	if ( !PG_ARGISNULL(2) )
		pszBackup = GET_STR(PG_GETARG_TEXT_P(2));
	
	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute( pszBackupDirectory );
	
	/*
	 * See whether the directory exists.  
	 * If not, try to create it.  
	 * If so, make sure it's a directory
	 */
	 validateBackupDirectory( pszBackupDirectory ); 
	
	/* Form backup file path name */
	char* pszFileName = formBackupFilePathName( pszBackupDirectory, pszBackupKey );
	
	/* Write file */
	FILE* f = fopen( pszFileName, "w" );
	if ( f == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Backup File %s cannot be opened", pszFileName )));
	}
	
	int nBytes = strlen(pszBackup); 
	if ( nBytes != fwrite( pszBackup, 1, nBytes, f ) )
	{
		fclose( f );
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				errmsg("Error writing Backup File %s", pszFileName)));
	}
	
	fclose( f );
	f = NULL;
	
	assert( pszFileName != NULL && pszFileName[0] != '\0' );
	int len_name = strlen(pszFileName);
	int len = len_name + VARHDRSZ;
	text* result = (text *) palloc(len);
	if ( result == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	VARATT_SIZEP(result) = len;

	/* Fill data field of result string... */
	char* ptr = VARDATA(result);
	memcpy(ptr, pszFileName, len_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * createBackupDirectory( char* pszPathName ) return bool
 *
 * This will create a directory if it doesn't exist.  If pszPathName exists, it just returns.
 *
 * It works by checking for the existence of the subdirectories. starting from the first
 * slash.  If the subdirectory doesn't exist, it creates it if it can, then moves on to the
 * next slash.
 */
bool createBackupDirectory( char* pszPathName )
{ 
	struct stat info;

	/* Does pszPathName already exist?  If so, return */
	if ( 0 == stat( pszPathName, &info ) )
		return true;
	
	/* Can we make it in its entirety?  If so, return */
	if ( 0 == mkdir( pszPathName, S_IRWXU ) )
		return true;
	
	/* Try to create each level of the hierarchy that doesn't already exist */
	char* pSlash = pszPathName+1;
	
	while ( pSlash != NULL && *pSlash != '\0' )
	{
		/* Find next subdirectory delimiter */
		pSlash = strchr( pSlash, '/' );
		if ( pSlash == NULL )
		{
			/* If no more, we try to create the whole thing. */
			if ( 0 > stat( pszPathName, &info ) )
			{
				if ( 0 != mkdir( pszPathName, S_IRWXU ) )
					return false;
			}
		}
		else
		{
			/* Temporarily end the string at the / we just found. */
			*pSlash = '\0';

			/* See whether this partial path exists. */
			if ( 0 > stat( pszPathName, &info ) )
			{
				/* If not, create it. */
				if ( 0 != mkdir( pszPathName, S_IRWXU ) )
				{
					*pSlash = '/';
					return false;
				}
			}
		
			/* Put back the slash we overwrote above. */
			*pSlash = '/';

			/* Advance past the slash. */
			pSlash++;
		}
	}
	
	return true;
}

/* findAcceptableBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey ) returns char*
 *
 * This function takes the directory and timestamp key and finds an existing file that matches
 * the naming convention for a backup file with the proper key and segid, but any instid.
 * In case it was backed up from a redundant instance of the same segment.
 */
char* findAcceptableBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey, int segid )
{
	/* Make sure that pszBackupDirectory exists and is in fact a directory. */
	struct stat		buf;
	struct dirent*	dirp = NULL;
	DIR*			dp = NULL;
	char*			pszBackupFilePathName = NULL;
	
	static regex_t rFindFileName;
	static bool bFirstTime = true;
	
	if ( bFirstTime )
	{
		char* pszRegex = (char *)palloc( 19 + 10 + strlen(pszBackupKey) + 1);
		if ( pszRegex == NULL )
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
			
		sprintf( pszRegex, "^cdb_dump_[0-9]+_%d_%s$", segid, pszBackupKey );   
		if ( 0 != regcomp( &rFindFileName, pszRegex, REG_EXTENDED ) )
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("Could not compile regular expression for backup filename matching")));
		}
		
		bFirstTime = false;
	}
	
	if ( lstat(pszBackupDirectory, &buf) < 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup Directory %s does not exist.", pszBackupDirectory)));
	}

	if ( S_ISDIR( buf.st_mode ) == 0 )
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup Location %s is not a directory.", pszBackupDirectory)));
	}
	
	dp = opendir(pszBackupDirectory);
	if ( dp == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup Directory %s cannot be opened for enumerating files.", pszBackupDirectory)));
	}
	
	while ( NULL != (dirp = readdir( dp )))
	{
		char* pszFileName = dirp->d_name;
			
		if ( 0 == regexec( &rFindFileName, pszFileName, 0, NULL, 0 ) )
		{
			char* pszSep = "/";
			if ( strlen(pszBackupDirectory) >= 1 && pszBackupDirectory[strlen(pszBackupDirectory) - 1] == '/' )
				pszSep = "";
			
			pszBackupFilePathName = (char *)palloc( strlen(pszBackupDirectory) + strlen(pszFileName) + strlen(pszSep) + 1 );
			sprintf( pszBackupFilePathName, "%s%s%s", pszBackupDirectory, pszSep, pszFileName );

			/* Make sure that this is a regular file */
			struct stat info;
			if ( 0 == stat( pszBackupFilePathName, &info ) && S_ISREG(info.st_mode))
			{
				break;
			}
		}
	}
	
	closedir( dp );
	return pszBackupFilePathName;
}

/* formBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey ) returns char*
 *
 * This function takes the directory and timestamp key and creates
 * the path and filename of the backup output file
 * based on the naming convention for this.
 */
char* formBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey )
{
	/* First form the prefix */
	char szFileNamePrefix[1+PATH_MAX];

    getCdbCdb();
    int instid  = (Cdb_segment==-1)?1:0; /*dispatch node*/ 
    int segid   = CdbIdentity.dbid;                                     
											
	sprintf( szFileNamePrefix, "cdb_dump_%d_%d_", instid, segid );
	
	/* Now add up the length of the pieces */
	int len = strlen(pszBackupDirectory);
	assert( len >= 1 );
	if ( pszBackupDirectory[ strlen(pszBackupDirectory)-1 ] != '/' )
		len++;
	 
	len += strlen(szFileNamePrefix); 
	len += strlen(pszBackupKey);
	
	if ( len > PATH_MAX )
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup FileName based on path %s and key %s too long", pszBackupDirectory, pszBackupKey)));
	}
	 
	char* pszBackupFileName = (char *) palloc( sizeof(char) * (1+len) );
	if ( pszBackupFileName == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	strcpy( pszBackupFileName, pszBackupDirectory);
	if ( pszBackupDirectory[ strlen(pszBackupDirectory)-1 ] != '/' )
		strcat( pszBackupFileName, "/" );
	
	strcat( pszBackupFileName, szFileNamePrefix );
	strcat( pszBackupFileName, pszBackupKey );
	
	return pszBackupFileName;
}

/* formStatusFilePathName( char* pszBackupDirectory, char* pszBackupKey, bool bIsBackup ) returns char*
 *
 * This function takes the directory and timestamp key and IsBackup flag and creates
 * the path and filename of the backup or restore status file
 * based on the naming convention for this.
 */
char* formStatusFilePathName( char* pszBackupDirectory, char* pszBackupKey, bool bIsBackup )
{
	/* First form the prefix */
	char szFileNamePrefix[1+PATH_MAX];

    getCdbCdb();
    int instid  = (Cdb_segment==-1)?1:0; /*dispatch node*/ 
    int segid   = CdbIdentity.dbid;                                     
											
	sprintf( szFileNamePrefix, "cdb_%s_status_%d_%d_", (bIsBackup ? "dump" : "restore"), 
						instid, segid );
	
	/* Now add up the length of the pieces */
	int len = strlen(pszBackupDirectory);
	assert( len >= 1 );
	if ( pszBackupDirectory[ strlen(pszBackupDirectory)-1 ] != '/' )
		len++;
	 
	len += strlen(szFileNamePrefix); 
	len += strlen(pszBackupKey);
	
	if ( len > PATH_MAX )
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Status FileName based on path %s and key %s too long", pszBackupDirectory, pszBackupKey)));
	}
	 
	char* pszFileName = (char *) palloc( sizeof(char) * (1+len) );
	if ( pszFileName == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	strcpy( pszFileName, pszBackupDirectory);
	if ( pszBackupDirectory[ strlen(pszBackupDirectory)-1 ] != '/' )
		strcat( pszFileName, "/" );
	
	strcat( pszFileName, szFileNamePrefix );
	strcat( pszFileName, pszBackupKey );
	
	return pszFileName;
}

/*
 * relativeToAbsolute( char* pszRelativeDirectory ) returns char*
 *
 * This will turn a relative path into an absolute one, based off of the Data Directory 
 * for this database.
 *
 * If the path isn't absolute, it is prefixed with DataDir/. 
 */
char* relativeToAbsolute( char* pszRelativeDirectory )
{
	if ( pszRelativeDirectory[0] == '/' )
		return pszRelativeDirectory;
	
	char* pszAbsolutePath = (char *) palloc( strlen(DataDir) + 1 + strlen(pszRelativeDirectory) + 1);
	if ( pszAbsolutePath == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	sprintf(pszAbsolutePath, "%s/%s", DataDir, pszRelativeDirectory);
	
	return pszAbsolutePath;
}

/* testCmdsExist( char* pszCompressionProgram ) returns void
 *
 * This function tests whether pszBackupProgram and the pszCompressionProgram
 * (if non-empty) are in the path.  If not, it calls ereport.
 */
void testCmdsExist( char* pszCompressionProgram, char* pszBackupProgram )
{
	// Does pszBackupProgram exist?
	if ( pszBackupProgram != NULL && 
		*pszBackupProgram != '\0' && 
		!testProgramExists( pszBackupProgram ) )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s not found in path", pszBackupProgram)));
	}
	
	// Does compression utility exist?
	if ( pszCompressionProgram != NULL && 
		*pszCompressionProgram != '\0' && 
		!testProgramExists(pszCompressionProgram) )
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s (compression utility) not found in path", pszCompressionProgram)));
	}
}

/* testProgramExists(char* pszProgramName) returns bool
 * This runs a shell with which pszProgramName > tempfile
 * piping the output to a temp file.
 * If the file is empty, then pszProgramName didn't exist 
 * on the path. 
 */
bool testProgramExists(char* pszProgramName)
{
	/* Now see what the length of the file is  - 0 means not there */
	struct stat buf;

	if ( pszProgramName == NULL || *pszProgramName == '\0' )
		return false;
	
	/* The pszProgramName might have command line arguments, so we need to
	 * stop at the first whitespace 
	 */
	char* pszProgramNameLocal = palloc( strlen(pszProgramName) + 1 );
	if ( pszProgramNameLocal == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	strcpy( pszProgramNameLocal, pszProgramName );
	
	char* p = pszProgramNameLocal;
	while ( *p != '\0' )
	{
		if ( *p == ' ' || *p == '\t' || *p == '\n' )
			break;
		
		p++;
	}
	
	*p = '\0';
	
	char* pszEnvPath = getenv( "PATH" );
	if ( pszEnvPath == NULL )
		return false;
	
	char* pszPath = (char *) palloc( strlen(pszEnvPath) + 1 );
	if ( pszPath == NULL )
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	strcpy( pszPath, pszEnvPath );
	
	/* Try to create each level of the hierarchy that doesn't already exist */
	char* pColon = pszPath;
	char* pColonNext;
	
	while ( pColon != NULL && *pColon != '\0' )
	{
		/* Find next delimiter */
		pColonNext = strchr( pColon, ':' );

		if ( pColonNext == pColon )
		{
			pColon++;
			continue;
		}
		
		if ( pColonNext == NULL )
		{
			// See whether pszProgramName exists in subdirectory pColon
			char* pszTestPath = (char *)palloc( strlen(pColon) + 1 + strlen(pszProgramNameLocal) + 1 );
			if ( pszTestPath == NULL )
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}
			
			sprintf( pszTestPath, "%s/%s", pColon, pszProgramNameLocal );
			if ( stat( pszTestPath, &buf ) >= 0 )
				return true;
			else
				return false;
		}

		/* Temporarily end the string at the : we just found. */
		*pColonNext = '\0';

		// See whether pszProgramName exists in subdirectory pColon
		char* pszTestPath = (char *)palloc( strlen(pColon) + 1 + strlen(pszProgramNameLocal) + 1 );
		if ( pszTestPath == NULL )
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}
		
		sprintf( pszTestPath, "%s/%s", pColon, pszProgramNameLocal );
		if ( stat( pszTestPath, &buf ) >= 0 )
		{
			*pColonNext = ':';
			return true;
		}
		
		/* Put back the colon we overwrote above. */
		*pColonNext = ':';

		/* Advance past the colon. */
		pColon = pColonNext + 1;
	}
	
	return false;
}

/* validateBackupDirectory( char* pszBackupDirectory ) returns void
 * This sees whether the backupDirectory exists
 * If so, it makes sure its a directory and not a file.
 * If not, it tries to create it.  If it cannot it calls ereport 
 */
void validateBackupDirectory( char* pszBackupDirectory )
{
	struct stat info;
	if ( stat( pszBackupDirectory, &info ) < 0 )
	{
        elog(DEBUG1, "Backup Directory %s does not exist", pszBackupDirectory );
		
		/* Attempt to create it. */
		if ( !createBackupDirectory( pszBackupDirectory ) )
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create backup directory \"%s\": %m",
							pszBackupDirectory)));
		}
		else
		{
			elog(DEBUG1, "Successfully created Backup Directory %s", pszBackupDirectory );
		}
	}
	else
	{
		if ( !S_ISDIR( info.st_mode ) )
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					errmsg("BackupDirectory %s exists but is not a directory.", pszBackupDirectory)));
		}
	}
}
