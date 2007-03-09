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
#include "regex/regex.h"
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
static bool createBackupDirectory(char *pszPathName);
static char *findAcceptableBackupFilePathName(char *pszBackupDirectory, char *pszBackupKey, int segid);
static char *formBackupFilePathName(char *pszBackupDirectory, char *pszBackupKey, bool is_compress);
static char *formStatusFilePathName(char *pszBackupDirectory, char *pszBackupKey, bool bIsBackup);
static char *relativeToAbsolute(char *pszRelativeDirectory);
static void testCmdsExist(char *pszCompressionProgram, char *pszBackupProgram);
static char *testProgramExists(char *pszProgramName);
static void validateBackupDirectory(char *pszBackupDirectory);

static char *compPg = NULL,
		   *bkPg = NULL;

typedef enum backup_file_type
{
	BFT_BACKUP = 0,
	BFT_BACKUP_STATUS = 1,
	BFT_RESTORE_STATUS = 2
} BackupFileType;

/*
 * mpp_backup_launch__( TEXT, TEXT, TEXT, TEXT, TEXT ) returns TEXT
 *
 * Called by mpp_dump.c to launch mpp_dump_agent on this host.
 */
PG_FUNCTION_INFO_V1(mpp_backup_launch__);
Datum
mpp_backup_launch__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list. */
	char	   *pszBackupDirectory = "./";
	char	   *pszBackupKey = "";
	char	   *pszCompressionProgram = "";
	char	   *pszPassThroughParameters = "";
	char	   *pszPassThroughCredentials = "";
	char	   *pszDataOption = "";
	char	   *pszBackupFileName,
			   *pszStatusFileName;
	FILE	   *ftest;
	char	   *pszDBName,
			   *pszUserName;
	pid_t		newpid;
	char	   *pszCmdLine;
	int			port;
	char	   *pszKeyParm;		/* dispatch node */
	int			segid;
	int			len,
				len_name;
	text	   *result;
	char	   *ptr;
	int			instid;			/* dispatch node */
	bool		is_compress;		

	if (!PG_ARGISNULL(0))
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if (*pszBackupDirectory == '\0')
			pszBackupDirectory = "./";
	}

	if (!PG_ARGISNULL(1))
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));

	if (!PG_ARGISNULL(2))
		pszCompressionProgram = GET_STR(PG_GETARG_TEXT_P(2));

	if (!PG_ARGISNULL(3))
		pszPassThroughParameters = GET_STR(PG_GETARG_TEXT_P(3));

	if (!PG_ARGISNULL(4))
		pszPassThroughCredentials = GET_STR(PG_GETARG_TEXT_P(4));

	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute(pszBackupDirectory);

	/*
	 * See whether the directory exists.
	 * If not, try to create it.
	 * If so, make sure it's a directory
	 */
	validateBackupDirectory(pszBackupDirectory);
	
	/*
	 * Validate existence of mpp_dump_agent and compression program, if
	 * possible
	 */
	testCmdsExist(pszCompressionProgram, "mpp_dump_agent");	
	
	/* are we going to use a compression program? */
	is_compress = (pszCompressionProgram != NULL && *pszCompressionProgram != '\0' ? true : false);

	/* Form backup file path name */
	pszBackupFileName = formBackupFilePathName(pszBackupDirectory, pszBackupKey, is_compress);
	pszStatusFileName = formStatusFilePathName(pszBackupDirectory, pszBackupKey, true);

	/*
	 * Make sure we can create this file before we spin off sh cause we don't
	 * get a good error message from sh if we can't write to the file
	 */
	ftest = fopen(pszBackupFileName, "w");
	if (ftest == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error creating file %s in mpp_backup_launch",
						pszBackupFileName)));
	}
	fclose(ftest);


	pszDBName = NULL;
	pszUserName = (char *) NULL;
	if (MyProcPort != NULL)
	{
		pszDBName = MyProcPort->database_name;
		pszUserName = MyProcPort->user_name;
	}

	if (pszDBName == NULL)
		pszDBName = "";
	if (pszUserName == NULL)
		pszUserName = "";

	/* Fork off mpp_dump_agent	*/
#ifdef _WIN32
	exit(1);
#else
	newpid = fork();
#endif
	if (newpid < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Could not fork a process for backup of database %s", pszDBName)));
	}
	else if (newpid == 0)
	{
		/* Child Process */
		port = PostPortNumber;

		/* Create the --mpp-k parameter string */
		pszKeyParm = (char *) palloc(strlen(pszBackupKey) +
									 strlen(pszPassThroughCredentials) +
									 3 + 10 + 10 + 1);
		if (pszKeyParm == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		getMppMpp();
		instid = (MppIdentity.segindex == -1) ? 1 : 0;	/* dispatch node */
		segid = MppIdentity.dbid;
		
		/* 
		 * dump schema and data for entry db, and data only for segment db.
		 * why dump data for entry db? because that will include statements
		 * to update the sequence tables (last values). As for actual data -
		 * there isn't any, so we will just dump empty COPY statements...
		 */
		pszDataOption = (instid == 1) ? "  " : "-a";
		
		sprintf(pszKeyParm, "%s_%d_%d_%s", pszBackupKey, instid, segid, pszPassThroughCredentials);

		/*
		 * In the following calc for the number of characters in the
		 * pszCmdLine, I show each piece as the count of the string for that
		 * piece  + 1 for the space after that piece, e.g. -p comes out 2 for
		 * the -p and 1 for the space after the -p	The last 1 is for the
		 * binary zero at the end.
		 */
		len = strlen(bkPg) + 1	/* mpp_dump_agent */
			+ 7 + 1				/* --mpp-k */
			+ strlen(pszKeyParm) + 1
			+ 7 + 1				/* --mpp-d */
			+ strlen(pszBackupDirectory) + 1
			+ 2 + 1				/* -p */
			+ 5 + 1				/* port */
			+ 2 + 1				/* -U */
			+ strlen(pszUserName) + 1	/* pszUserName */
			+ strlen(pszPassThroughParameters) + 1
			+ 2 + 1				/* "  " or "-a" */
			+ strlen(pszDBName) + 1
			+ 1 + 1				/* > */
			+ strlen(pszBackupFileName)
			+ 2 + 1				/* 2> */
			+ strlen(pszStatusFileName)
			+ 1;

		/* if user selected a compression program */
		if (pszCompressionProgram[0] != '\0')
		{
			len += 1 + 1		/* | */
			+ strlen(compPg) + 1;

			pszCmdLine = (char *) palloc(len);
			if (pszCmdLine == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			/* mpp_dump_agent + options, pipe into compression program, direct
			 * stdout to backup file and stderr to status file */
			//sprintf(pszCmdLine, "%s --mpp-k %s --mpp-d %s -p %d -U %s %s %s %s | %s > %s 2> %s",
			sprintf(pszCmdLine, "%s --mpp-k %s --mpp-d %s -p %d -U %s %s %s %s 2> %s | %s > %s",
					bkPg, pszKeyParm, pszBackupDirectory, port, pszUserName, pszPassThroughParameters, 
					pszDataOption, pszDBName, pszStatusFileName, compPg, pszBackupFileName);
		}
		else
		{
			pszCmdLine = (char *) palloc(len);
			if (pszCmdLine == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			sprintf(pszCmdLine, "%s --mpp-k %s --mpp-d %s -p %d -U %s %s %s %s > %s 2> %s",
					bkPg, pszKeyParm, pszBackupDirectory, port, pszUserName, pszPassThroughParameters, pszDataOption, pszDBName,
					pszBackupFileName, pszStatusFileName);
		}

		elog(LOG, "mpp_dump_agent command line: %s", pszCmdLine),

		/* This execs a shell that runs the mpp_dump_agent program	*/
			execl("/bin/sh", "sh", "-c", pszCmdLine, NULL);

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error in mpp_backup_launch - execl of %s with Command Line %s failed",
						"/bin/sh", pszCmdLine)));
		_exit(127);
	}

	assert(pszBackupFileName != NULL && pszBackupFileName[0] != '\0');
	len_name = strlen(pszBackupFileName);
	len = len_name + VARHDRSZ;
	result = (text *) palloc(len);
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	memcpy(ptr, pszBackupFileName, len_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * mpp_restore_launch__( TEXT, TEXT, TEXT, TEXT, TEXT, TEXT, INT, bool ) returns TEXT
 *
 * Called by mpp_restore.c to launch mpp_restore_agent on this host.
 */
PG_FUNCTION_INFO_V1(mpp_restore_launch__);
Datum
mpp_restore_launch__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list. */
	char	   *pszBackupDirectory = "./";
	char	   *pszBackupKey = "";
	char	   *pszCompressionProgram = "";
	char	   *pszPassThroughParameters = "";
	char	   *pszPassThroughCredentials = "";
	char	   *pszPassThroughTargetInfo = "";
	bool		bOnErrorStop = false;
	int			instid;			/* dispatch node */
	int			segid;
	int			target_dbid;
	char	   *pszOnErrorStop;
	char	   *pszBackupFileName;
	char	   *pszStatusFileName;
	struct stat info;
	char	   *pszDBName;
	char	   *pszUserName;
	pid_t		newpid;
	char	   *pszCmdLine;
	int			port;
	char	   *pszKeyParm;
	int			len,
				len_name;
	text	   *result;
	char	   *ptr;
	bool		is_compress;

	getMppMpp();
	instid = (MppIdentity.segindex == -1) ? 1 : 0;		/* dispatch node */
	segid = MppIdentity.dbid;

	if (!PG_ARGISNULL(0))
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if (*pszBackupDirectory == '\0')
			pszBackupDirectory = "./";
	}

	if (!PG_ARGISNULL(1))
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));

	if (!PG_ARGISNULL(2))
		pszCompressionProgram = GET_STR(PG_GETARG_TEXT_P(2));

	if (!PG_ARGISNULL(3))
		pszPassThroughParameters = GET_STR(PG_GETARG_TEXT_P(3));

	if (!PG_ARGISNULL(4))
		pszPassThroughCredentials = GET_STR(PG_GETARG_TEXT_P(4));

	if (!PG_ARGISNULL(5))
		pszPassThroughTargetInfo = GET_STR(PG_GETARG_TEXT_P(5));
	
	if (!PG_ARGISNULL(6))
		target_dbid = PG_GETARG_INT32(6);
	
	if (!PG_ARGISNULL(7))
		bOnErrorStop = PG_GETARG_BOOL(7);

	pszOnErrorStop = bOnErrorStop ? "--mpp-e" : "";

	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute(pszBackupDirectory);

	/* Validate existence of compression program and mpp_restore_agent program */
	testCmdsExist(pszCompressionProgram, "mpp_restore_agent");

	/* are we going to use a compression program? */
	is_compress = (pszCompressionProgram != NULL && *pszCompressionProgram != '\0' ? true : false);

	/* Form backup file path name */
	pszBackupFileName = formBackupFilePathName(pszBackupDirectory, pszBackupKey, is_compress);
	pszStatusFileName = formStatusFilePathName(pszBackupDirectory, pszBackupKey, false);

	/* Make sure pszBackupFileName exists */
	if (0 != stat(pszBackupFileName, &info))
	{
		pszBackupFileName = findAcceptableBackupFilePathName(pszBackupDirectory, pszBackupKey, segid);
		if (pszBackupFileName == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("No Backup FileName could be found to restore dbid %d from directory %s with backup key %s", segid, pszBackupDirectory, pszBackupKey)));
	}

	pszDBName = NULL;
	pszUserName = NULL;
	if (MyProcPort != NULL)
	{
		pszDBName = MyProcPort->database_name;
		pszUserName = MyProcPort->user_name;
	}

	if (pszDBName == NULL)
		pszDBName = "";
	if (pszUserName == NULL)
		pszUserName = "";

	/* Fork off mpp_restore_agent  */
#ifdef _WIN32
	exit(1);
#else
	newpid = fork();
#endif
	if (newpid < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Could not fork a process for backup of database %s", pszDBName)));
	}
	else if (newpid == 0)
	{
		/* Child Process */
		port = PostPortNumber;

		/* Create the --mpp-k parameter string */
		pszKeyParm = (char *) palloc(strlen(pszBackupKey) +
									 strlen(pszPassThroughCredentials) +
									 3 + 10 + 10 + 1);
		if (pszKeyParm == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		sprintf(pszKeyParm, "%s_%d_%d_%s", pszBackupKey, instid, segid, pszPassThroughCredentials);

		/*
		 * In the following calc for the number of characters in the
		 * pszCmdLine, I show each piece as the count of the string for that
		 * piece  + 1 for the space after that piece, e.g. -p comes out 2 for
		 * the -p and 1 for the space after the -p	The last 1 is for the
		 * binary zero at the end.
		 */
		len = strlen(bkPg) + 1	/* mpp_restore_agent */
			+ 7 + 1				/* --mpp-k */
			+ strlen(pszKeyParm) + 1
			+ 7 + 1				/* --mpp-d */
			+ strlen(pszBackupDirectory) + 1
			+ 7 + 1				/* --mpp-e */
			+ 2 + 1				/* -p */
			+ 5 + 1				/* port */
			+ 2 + 1				/* -U */
			+ strlen(pszUserName) + 1	/* pszUserName */
			+ strlen(pszPassThroughParameters) + 1
			+ strlen(pszPassThroughTargetInfo) + 1
			+ 2 + 1				/* -d */
			+ strlen(pszDBName) + 1
			+ 1 + 1				/* > */
			+ strlen(pszStatusFileName)
			+ 4 + 1				/* 2>&1 */
			+ 1;

		/* 
		 * if compression was requested with --mpp-c
		 */
		if (pszCompressionProgram[0] != '\0')
		{
			len += 4 + strlen(compPg) +
				+strlen(pszBackupFileName);
			
			len += 8; /* pass along "--mpp-c" to tell restore file is compressed */
			
			pszCmdLine = (char *) palloc(len);
			if (pszCmdLine == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			/* 
			 * de-compress backupfile and pipe into stdin of mpp_restore_agent along with its
			 * options. Redirect both stdout and stderr into the status file.
			 */
			//sprintf(pszCmdLine, "%s %s | %s --mpp-c --mpp-k %s --mpp-d %s %s -p %d -U %s %s %s -d %s > %s 2>&1",
			//		compPg, pszBackupFileName, bkPg, pszKeyParm, pszBackupDirectory, pszOnErrorStop, port, pszUserName,
			//		pszPassThroughParameters, pszPassThroughTargetInfo, pszDBName, pszStatusFileName);
			sprintf(pszCmdLine, "%s --mpp-c %s --mpp-k %s --mpp-d %s %s -p %d -U %s %s %s -d %s %s > %s 2>&1",
					bkPg, compPg, pszKeyParm, pszBackupDirectory, pszOnErrorStop, port, pszUserName,
					pszPassThroughParameters, pszPassThroughTargetInfo, pszDBName, pszBackupFileName, pszStatusFileName);
			
		}
		else
		{
			len += strlen(pszBackupFileName) + 1;

			pszCmdLine = (char *) palloc(len);
			if (pszCmdLine == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			/* Format mpp_restore_agent with options, and Redirect both stdout and stderr into the status file */
			sprintf(pszCmdLine, "%s --mpp-k %s --mpp-d %s %s -p %d -U %s %s %s -d %s %s > %s 2>&1",
					bkPg, pszKeyParm, pszBackupDirectory, pszOnErrorStop, port, pszUserName, pszPassThroughParameters, 
					pszPassThroughTargetInfo, pszDBName, pszBackupFileName, pszStatusFileName);
		}


		elog(LOG, "mpp_restore_agent command line: %s", pszCmdLine),

		/* This execs a shell that runs the mpp_restore_agent program  */
			execl("/bin/sh", "sh", "-c", pszCmdLine, NULL);

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error in mpp_restore_launch - execl of %s with Command Line %s failed",
						"/bin/sh", pszCmdLine)));
		_exit(127);
	}

	assert(pszBackupFileName != NULL && pszBackupFileName[0] != '\0');

	len_name = strlen(pszBackupFileName);
	len = len_name + VARHDRSZ;
	result = (text *) palloc(len);
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	memcpy(ptr, pszBackupFileName, len_name);

	PG_RETURN_TEXT_P(result);
}

/*
 * mpp_read_backup_file__( TEXT, TEXT, int ) returns TEXT
 *
 */
PG_FUNCTION_INFO_V1(mpp_read_backup_file__);
Datum
mpp_read_backup_file__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list. */
	char	   *pszBackupDirectory = "./";
	char	   *pszBackupKey = "";
	int			fileType = 0;
	char	   *pszFileName;
	struct stat info;
	char	   *pszBackup;
	FILE	   *f;
	int			len,
				len_status;
	text	   *result;
	char	   *ptr;

	if (!PG_ARGISNULL(0))
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if (*pszBackupDirectory == '\0')
			pszBackupDirectory = "./";
	}

	if (!PG_ARGISNULL(1))
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));

	if (!PG_ARGISNULL(2))
		fileType = PG_GETARG_INT32(2);

	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute(pszBackupDirectory);

	/* Form backup file path name */
	pszFileName = NULL;
	switch (fileType)
	{
		case BFT_BACKUP:
			/*pszFileName = formBackupFilePathName(pszBackupDirectory, pszBackupKey);*/
			/*we can't support this without making a change to this function arguments, which
			  will require an initdb, which we want to avoid. This is due to the fact that in
			  this call to formBackupFilePathName we don't know if the file is compressed or
			  not and formBackupFilePathName will not know if to create a file with a compression
			  suffix or not */
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("Only status files are currently supported in mpp_read_backup_file")));			
			break;
		case BFT_BACKUP_STATUS:
		case BFT_RESTORE_STATUS:
			pszFileName = formStatusFilePathName(pszBackupDirectory, pszBackupKey, (fileType == BFT_BACKUP_STATUS));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("Invalid filetype %d passed to mpp_read_backup_file", fileType)));
			break;
	}

	/* Make sure pszFileName exists */
	if (0 != stat(pszFileName, &info))
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Backup File %s Type %d could not be be found", pszFileName, fileType)));
	}

	/* Read file */
	if (info.st_size > INT_MAX)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Backup File %s Type %d too large to read", pszFileName, fileType)));
	}

	/* Allocate enough memory for entire file, and read it in. */
	pszBackup = (char *) palloc(info.st_size + 1);
	if (pszBackup == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	f = fopen(pszFileName, "r");
	if (f == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Backup File %s Type %d cannot be opened", pszFileName, fileType)));
	}

	if (info.st_size != fread(pszBackup, 1, info.st_size, f))
	{
		fclose(f);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error reading Backup File %s Type %d", pszFileName, fileType)));
	}

	fclose(f);
	f = NULL;
	pszBackup[info.st_size] = '\0';

	len_status = strlen(pszBackup);
	len = len_status + VARHDRSZ;
	result = (text *) palloc(len);
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	/* Set size of result string... */
	SET_VARSIZE(result, len);

	/* Fill data field of result string... */
	ptr = VARDATA(result);
	memcpy(ptr, pszBackup, len_status);

	PG_RETURN_TEXT_P(result);
}

/*
 * mpp_write_backup_file__( TEXT, TEXT, TEXT ) returns TEXT
 *
 */

/* NOT IN USE ANYMORE! */
 
/*
 * mpp_write_backup_file__( TEXT, TEXT, TEXT ) returns TEXT
 *
 */
/* 
 * MPPTODO: This function assumes the file is not compressed.
 * This is not always the case. Therefore it should be used carefully
 * by the caller, until we decide to make the change and pass in an
 * isCompressed boolean, which unfortunately will require an initdb for
 * the change to take effect.
 */
PG_FUNCTION_INFO_V1(mpp_write_backup_file__);
Datum
mpp_write_backup_file__(PG_FUNCTION_ARGS)
{
	/* Fetch the parameters from the argument list. */
	char	   *pszBackupDirectory = "./";
	char	   *pszBackupKey = "";
	char	   *pszBackup = "";
	char	   *pszFileName;
	FILE	   *f;
	int			nBytes;
	int			len,
		len_name;
	text	   *result;
	char	   *ptr;
	
	if (!PG_ARGISNULL(0))
	{
		pszBackupDirectory = GET_STR(PG_GETARG_TEXT_P(0));
		if (*pszBackupDirectory == '\0')
			pszBackupDirectory = "./";
	}
	
	if (!PG_ARGISNULL(1))
		pszBackupKey = GET_STR(PG_GETARG_TEXT_P(1));
	
	if (!PG_ARGISNULL(2))
		pszBackup = GET_STR(PG_GETARG_TEXT_P(2));
	
	/*
	 * if BackupDirectory is relative, make it absolute based on the directory
	 * where the database resides
	 */
	pszBackupDirectory = relativeToAbsolute(pszBackupDirectory);
	
	/*
	 * See whether the directory exists.
	 * If not, try to create it.
	 * If so, make sure it's a directory
	 */
	validateBackupDirectory(pszBackupDirectory);
	
	/* Form backup file path name */
	pszFileName = formBackupFilePathName(pszBackupDirectory, pszBackupKey, false);
	
	/* Write file */
	f = fopen(pszFileName, "w");
	if (f == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Backup File %s cannot be opened", pszFileName)));
	}
	
	nBytes = strlen(pszBackup);
	if (nBytes != fwrite(pszBackup, 1, nBytes, f))
	{
		fclose(f);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("Error writing Backup File %s", pszFileName)));
	}
	
	fclose(f);
	f = NULL;
	
	assert(pszFileName != NULL && pszFileName[0] != '\0');
	len_name = strlen(pszFileName);
	len = len_name + VARHDRSZ;
	result = (text *) palloc(len);
	if (result == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	
	/* Set size of result string... */
	SET_VARSIZE(result, len);
	
	/* Fill data field of result string... */
	ptr = VARDATA(result);
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
bool
createBackupDirectory(char *pszPathName)
{
	struct stat info;
	char	   *pSlash;

	/* Does pszPathName already exist?	If so, return */
	if (0 == stat(pszPathName, &info))
		return true;

	/* Can we make it in its entirety?	If so, return */
	if (0 == mkdir(pszPathName, S_IRWXU))
		return true;

	/* Try to create each level of the hierarchy that doesn't already exist */
	pSlash = pszPathName + 1;

	while (pSlash != NULL && *pSlash != '\0')
	{
		/* Find next subdirectory delimiter */
		pSlash = strchr(pSlash, '/');
		if (pSlash == NULL)
		{
			/* If no more, we try to create the whole thing. */
			if (0 > stat(pszPathName, &info))
			{
				if (0 != mkdir(pszPathName, S_IRWXU))
					return false;
			}
		}
		else
		{
			/* Temporarily end the string at the / we just found. */
			*pSlash = '\0';

			/* See whether this partial path exists. */
			if (0 > stat(pszPathName, &info))
			{
				/* If not, create it. */
				if (0 != mkdir(pszPathName, S_IRWXU))
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
char *
findAcceptableBackupFilePathName(char *pszBackupDirectory, char *pszBackupKey, int segid)
{
	/* Make sure that pszBackupDirectory exists and is in fact a directory. */
	struct stat buf;
	struct dirent *dirp = NULL;
	DIR		   *dp = NULL;
	char	   *pszBackupFilePathName = NULL;

	static regex_t rFindFileName;
	static bool bFirstTime = true;
	char	   *pszRegex;
	char	   *pszFileName;
	char	   *pszSep;
	struct stat info;

	if (bFirstTime)
	{
		int			wmasklen,
						masklen;
			pg_wchar   *mask;
		pszRegex = (char *) palloc(19 + 10 + strlen(pszBackupKey) + 1);
		if (pszRegex == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		sprintf(pszRegex, "^mpp_dump_[0-9]+_%d_%s(.gz)?$", segid, pszBackupKey);
		
		masklen = strlen(pszRegex);
		mask = (pg_wchar *) palloc((masklen + 1) * sizeof(pg_wchar));
		wmasklen = pg_mb2wchar_with_len(pszRegex, mask, masklen);

		if (0 != pg_regcomp(&rFindFileName, mask, wmasklen, REG_EXTENDED))
		{
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("Could not compile regular expression for backup filename matching")));
		}

		bFirstTime = false;
	}

	if (lstat(pszBackupDirectory, &buf) < 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		 errmsg("Backup Directory %s does not exist.", pszBackupDirectory)));
	}

	if (S_ISDIR(buf.st_mode) == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup Location %s is not a directory.", pszBackupDirectory)));
	}

	dp = opendir(pszBackupDirectory);
	if (dp == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup Directory %s cannot be opened for enumerating files.", pszBackupDirectory)));
	}

	while (NULL != (dirp = readdir(dp)))
	{
		pg_wchar   *data;
		size_t		data_len;
		int			newfile_len;
		pszFileName = dirp->d_name;

		/* Convert data string to wide characters */
		newfile_len = strlen(pszFileName);
		data = (pg_wchar *) palloc((newfile_len + 1) * sizeof(pg_wchar));
		data_len = pg_mb2wchar_with_len(pszFileName, data, newfile_len);

		if (0 == pg_regexec(&rFindFileName, data, data_len, 0, NULL, 0, NULL, 0))
		{
			pszSep = "/";
			if (strlen(pszBackupDirectory) >= 1 && pszBackupDirectory[strlen(pszBackupDirectory) - 1] == '/')
				pszSep = "";

			pszBackupFilePathName = (char *) palloc(strlen(pszBackupDirectory) + strlen(pszFileName) + strlen(pszSep) + 1);
			sprintf(pszBackupFilePathName, "%s%s%s", pszBackupDirectory, pszSep, pszFileName);

			/* Make sure that this is a regular file */
			if (0 == stat(pszBackupFilePathName, &info) && S_ISREG(info.st_mode))
			{
				break;
			}
		}
	}

	closedir(dp);
	return pszBackupFilePathName;
}

/* formBackupFilePathName( char* pszBackupDirectory, char* pszBackupKey ) returns char*
 *
 * This function takes the directory and timestamp key and creates
 * the path and filename of the backup output file
 * based on the naming convention for this.
 */
char *
formBackupFilePathName(char *pszBackupDirectory, char *pszBackupKey, bool is_compress)
{
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
	/* First form the prefix */
	char		szFileNamePrefix[1 + PATH_MAX];
	int			instid;			/* dispatch node */
	int			segid;
	int			len;
	char	   *pszBackupFileName;

	getMppMpp();
	instid = (MppIdentity.segindex == -1) ? 1 : 0;		/* dispatch node */
	segid = MppIdentity.dbid;

	sprintf(szFileNamePrefix, "mpp_dump_%d_%d_", instid, segid);

	/* Now add up the length of the pieces */
	len = strlen(pszBackupDirectory);
	assert(len >= 1);
	if (pszBackupDirectory[strlen(pszBackupDirectory) - 1] != '/')
		len++;

	len += strlen(szFileNamePrefix);
	len += strlen(pszBackupKey);
	
	/* if gzip/gunzip is used, the suffix is gz */
	if(is_compress)
		len += strlen(".gz");
	
	if (len > PATH_MAX)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Backup FileName based on path %s and key %s too long", pszBackupDirectory, pszBackupKey)));
	}

	pszBackupFileName = (char *) palloc(sizeof(char) * (1 + len));
	if (pszBackupFileName == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	strcpy(pszBackupFileName, pszBackupDirectory);
	if (pszBackupDirectory[strlen(pszBackupDirectory) - 1] != '/')
		strcat(pszBackupFileName, "/");

	strcat(pszBackupFileName, szFileNamePrefix);
	strcat(pszBackupFileName, pszBackupKey);

	/* if gzip/gunzip is used, the suffix is gz */
	if(is_compress)
		strcat(pszBackupFileName, ".gz");
	
	return pszBackupFileName;
}

/* formStatusFilePathName( char* pszBackupDirectory, char* pszBackupKey, bool bIsBackup ) returns char*
 *
 * This function takes the directory and timestamp key and IsBackup flag and creates
 * the path and filename of the backup or restore status file
 * based on the naming convention for this.
 */
char *
formStatusFilePathName(char *pszBackupDirectory, char *pszBackupKey, bool bIsBackup)
{
	/* First form the prefix */
	char		szFileNamePrefix[1 + PATH_MAX];
	int			instid;			/* dispatch node */
	int			segid;
	int			len;
	char	   *pszFileName;

	getMppMpp();
	instid = (MppIdentity.segindex == -1) ? 1 : 0;		/* dispatch node */
	segid = MppIdentity.dbid;

	sprintf(szFileNamePrefix, "mpp_%s_status_%d_%d_", (bIsBackup ? "dump" : "restore"),
			instid, segid);

	/* Now add up the length of the pieces */
	len = strlen(pszBackupDirectory);
	assert(len >= 1);
	if (pszBackupDirectory[strlen(pszBackupDirectory) - 1] != '/')
		len++;

	len += strlen(szFileNamePrefix);
	len += strlen(pszBackupKey);

	if (len > PATH_MAX)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Status FileName based on path %s and key %s too long", pszBackupDirectory, pszBackupKey)));
	}

	pszFileName = (char *) palloc(sizeof(char) * (1 + len));
	if (pszFileName == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	strcpy(pszFileName, pszBackupDirectory);
	if (pszBackupDirectory[strlen(pszBackupDirectory) - 1] != '/')
		strcat(pszFileName, "/");

	strcat(pszFileName, szFileNamePrefix);
	strcat(pszFileName, pszBackupKey);

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
char *
relativeToAbsolute(char *pszRelativeDirectory)
{
	char	   *pszAbsolutePath;

	if (pszRelativeDirectory[0] == '/')
		return pszRelativeDirectory;

	pszAbsolutePath = (char *) palloc(strlen(DataDir) + 1 + strlen(pszRelativeDirectory) + 1);
	if (pszAbsolutePath == NULL)
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
 * (if non-empty) are in the path.	If not, it calls ereport.
 */
void
testCmdsExist(char *pszCompressionProgram, char *pszBackupProgram)
{
	/* Does pszBackupProgram exist? */
	if (pszBackupProgram != NULL &&
		*pszBackupProgram != '\0' &&
		(bkPg = testProgramExists(pszBackupProgram)) == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s not found in PGPATH or PATH (PGPATH: %s | PATH:  %s)",
						pszBackupProgram, getenv("PGPATH"), getenv("PATH")),
				 errhint("Restart the server and try again")));
	}

	/* Does compression utility exist? */
	if (pszCompressionProgram != NULL &&
		*pszCompressionProgram != '\0' &&
		(compPg = testProgramExists(pszCompressionProgram)) == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("%s (compression utility) not found in in PGPATH or "
						"PATH (PGPATH: %s | PATH:  %s)", 
						pszCompressionProgram, getenv("PGPATH"), getenv("PATH"))));
	}
}

/* testProgramExists(char* pszProgramName) returns bool
 * This runs a shell with which pszProgramName > tempfile
 * piping the output to a temp file.
 * If the file is empty, then pszProgramName didn't exist
 * on the path.
 */
char *
testProgramExists(char *pszProgramName)
{
	char	   *pszProgramNameLocal;
	char	   *p;

	/* Now see what the length of the file is  - 0 means not there */
	struct stat buf;
	char	   *pszEnvPath;
	char	   *pszPath;
	char	   *pColon;
	char	   *pColonNext;
	char	   *pszTestPath;

	if (pszProgramName == NULL || *pszProgramName == '\0')
		return NULL;

	/*
	 * The pszProgramName might have command line arguments, so we need to
	 * stop at the first whitespace
	 */
	pszProgramNameLocal = palloc(strlen(pszProgramName) + 1);
	if (pszProgramNameLocal == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	strcpy(pszProgramNameLocal, pszProgramName);

	p = pszProgramNameLocal;
	while (*p != '\0')
	{
		if (*p == ' ' || *p == '\t' || *p == '\n')
			break;

		p++;
	}

	*p = '\0';

	pszEnvPath = getenv("PGPATH");
	if (pszEnvPath != NULL)
	{
		pszTestPath = (char *) palloc(strlen(pszEnvPath) + 1 + strlen(pszProgramNameLocal) + 1);
		if (pszTestPath == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		sprintf(pszTestPath, "%s/%s", pszEnvPath, pszProgramNameLocal);
		if (stat(pszTestPath, &buf) >= 0)
			return pszTestPath;
	}
	pszEnvPath = getenv("PATH");
	if (pszEnvPath == NULL)
		return NULL;
	pszPath = (char *) palloc(strlen(pszEnvPath) + 1);
	if (pszPath == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}

	strcpy(pszPath, pszEnvPath);
	/* Try to create each level of the hierarchy that doesn't already exist */
	pColon = pszPath;

	while (pColon != NULL && *pColon != '\0')
	{
		/* Find next delimiter */
		pColonNext = strchr(pColon, ':');

		if (pColonNext == pColon)
		{
			pColon++;
			continue;
		}

		if (pColonNext == NULL)
		{
			/* See whether pszProgramName exists in subdirectory pColon */
			pszTestPath = (char *) palloc(strlen(pColon) + 1 + strlen(pszProgramNameLocal) + 1);
			if (pszTestPath == NULL)
			{
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
			}

			sprintf(pszTestPath, "%s/%s", pColon, pszProgramNameLocal);
			if (stat(pszTestPath, &buf) >= 0)
				return pszTestPath;
			else
				return NULL;
		}

		/* Temporarily end the string at the : we just found. */
		*pColonNext = '\0';

		/* See whether pszProgramName exists in subdirectory pColon */
		pszTestPath = (char *) palloc(strlen(pColon) + 1 + strlen(pszProgramNameLocal) + 1);
		if (pszTestPath == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		sprintf(pszTestPath, "%s/%s", pColon, pszProgramNameLocal);
		if (stat(pszTestPath, &buf) >= 0)
		{
			*pColonNext = ':';
			return pszTestPath;
		}

		/* Put back the colon we overwrote above. */
		*pColonNext = ':';

		/* Advance past the colon. */
		pColon = pColonNext + 1;

		/* free */
		pfree(pszTestPath);
	}

	return NULL;
}

/* validateBackupDirectory( char* pszBackupDirectory ) returns void
 * This sees whether the backupDirectory exists
 * If so, it makes sure its a directory and not a file.
 * If not, it tries to create it.  If it cannot it calls ereport
 */
void
validateBackupDirectory(char *pszBackupDirectory)
{
	struct stat info;

	if (stat(pszBackupDirectory, &info) < 0)
	{
		elog(DEBUG1, "Backup Directory %s does not exist", pszBackupDirectory);

		/* Attempt to create it. */
		if (!createBackupDirectory(pszBackupDirectory))
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create backup directory \"%s\": %m",
							pszBackupDirectory)));
		}
		else
		{
			elog(DEBUG1, "Successfully created Backup Directory %s", pszBackupDirectory);
		}
	}
	else
	{
		if (!S_ISDIR(info.st_mode))
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("BackupDirectory %s exists but is not a directory.", pszBackupDirectory)));
		}
	}
}
