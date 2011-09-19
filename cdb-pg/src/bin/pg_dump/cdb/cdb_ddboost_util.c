/*-------------------------------------------------------------------------
 *
 * gpddboost.c
 *	  gpddboost is a utility for accessing Data Domain's ddboost 
 *        interfaces.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <ctype.h>
#include <pthread.h>
#include "getopt_long.h"
#include "pqexpbuffer.h"
#include "libpq-fe.h"
#include "fe-auth.h"
#include "pg_backup.h"
#include "cdb_table.h"
#include "cdb_dump_util.h"
#include "cdb_backup_state.h"
#include "cdb_dump.h"
#include <sys/stat.h>
#include <sys/types.h>

#ifdef USE_DDBOOST
#include "ddp_api.h"

#define MAX_PATH_NAME 1024
#define DDB_IO_SIZE (1024 * 512)
#define DDB_POOL_SIZE (32 * 1024 * 2048)

/* This is necessary on platforms where optreset variable is not available.
 * Look at "man getopt" documentation for details.
 */
#ifndef HAVE_OPTRESET
int			optreset;
#endif

static ddp_inst_desc_t ddp_inst = DDP_INVALID_DESCRIPTOR;
static ddp_conn_desc_t ddp_conn = DDP_INVALID_DESCRIPTOR;

struct ddboost_options
{
	char *timestamp;
	char *ddb_username;
	char *ddb_password;
	char *ddb_hostname;
	char *directory;	
	char *from_file;
	char *to_file;
	char *deleteDir;
	char *deleteFile;
	char *table;
	char *database;
	bool listDir;
	bool copyFromDDBoost;	
	bool copyToDDBoost;
	bool readFile;	
	bool getFreePercent;
	bool getLatestTimestamp;
	bool syncFiles;
};

static struct ddboost_options *dd_options = NULL;
static void _ddp_test_log(const void *session_ptr, const ddp_char_t *log_msg, ddp_severity_t severity);

/* Forward decls */
static bool fillInputOptions(int argc, char **argv, InputOptions * pInputOpts);
static bool parmValNeedsQuotes(const char *Value);
static void usage(void);
static int readFromDDFile(FILE *fp, char *ddBoostFileName);	
static int readFromDDFileToOutput(char *ddBoostFileName);
static int readFromLocalFileToOutput(char *FileName);
static int writeToDDFile(FILE *fp, char *ddBoostFileName);	
static int initDDSystem(const char *dd_boost_username, const char *dd_boost_passwd, const char *dd_boost_hostname);

static int reportRestoreResults(const char *pszReportDirectory, const ThreadParm * pMasterParm, const ThreadParmArray * pParmAr);
static int reportMasterError(InputOptions inputopts, const ThreadParm * pMasterParm, const char *localMsg);
typedef struct option optType;

static RestoreOptions *opts = NULL;

static const char *logInfo = "INFO";
static const char *logError = "ERROR";
const char *progname;

static char * addPassThroughLongParm(const char *Parm, const char *pszValue, char *pszPassThroughParmString);
static char * shellEscape(const char *shellArg, PQExpBuffer escapeBuf);
static int dd_boost_enabled = 0;

static ddp_client_info_t cl_info = {DDP_CL_DDP, "Test DD Client"};

static int readFileFromDDBoost(struct ddboost_options *dd_options);
static int copyFileFromDDBoost(struct ddboost_options *dd_options);
static int copyFileToDDBoost(struct ddboost_options *dd_options);
static int ddBoostRmdir(char *dir_path, ddp_conn_desc_t ddp_conn, char *parent_dir);
static int deleteDir(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int deleteFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static float getFreePercent(struct ddboost_options *dd_options);
static int createDDboostDir(ddp_conn_desc_t ddp_conn, char *storage_unit_name, const char *path_name);
static int listDirectory(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int createFakeRestoreFile(char *schemaTable, struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn); 
static int splitSchemaAndTableName(char *schemaTable, char **schema, char **table);
static int lineStartsWith(char *line, char *pattern);
static int lineEndsWith(char *line, char *pattern);
static int getLatestTimestamp(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath, char *database);
static int dumpFileHasDatabaseName(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath, char *database);
static int syncFilesFromDDBoost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath);
static int isFileToBeCopied(const char*);
static int copyFilesFromDir(const char *fromDir, char *toDir, ddp_conn_desc_t ddp_conn);

int
main(int argc, char **argv)
{
	int			failCount = 0;

	/* This struct holds the values of the command line parameters */
	InputOptions inputOpts;

	dd_options = (struct ddboost_options*)malloc(sizeof(struct ddboost_options));
	memset(dd_options, 0, sizeof(struct ddboost_options));

	progname = get_progname(argv[0]);

	/* Parse command line for options */
	if (!fillInputOptions(argc, argv, &inputOpts))
	{
		failCount = reportMasterError(inputOpts, NULL, get_early_error());
		mpp_err_msg(logInfo, progname, "Reporting Master Error from fillInputOptions.\n");
		goto cleanup;
	}

	if (dd_options->readFile && (!dd_options->ddb_username))
	{
		/* This is a read from local file */
		failCount = readFromLocalFileToOutput(dd_options->from_file);
		goto cleanup;
	}

	if (!dd_options->ddb_username)
	{
		mpp_err_msg(logInfo, progname, "ddboost username is NULL");
		goto cleanup;
	}
	if (!dd_options->ddb_password)
	{
		mpp_err_msg(logInfo, progname, "ddboost password is NULL");
		goto cleanup;
	}
	if (!dd_options->ddb_hostname)
	{
		mpp_err_msg(logInfo, progname, "ddboost hostname is NULL");
		goto cleanup;
	}

	initDDSystem(dd_options->ddb_username, dd_options->ddb_password, dd_options->ddb_hostname);

	/* Take appropriate actions here */
	if (dd_options->copyFromDDBoost)
	{
		copyFileFromDDBoost(dd_options);
		goto cleanup;
	}

	if (dd_options->copyToDDBoost)
	{
		copyFileToDDBoost(dd_options);
		goto cleanup;
	}

	if (dd_options->deleteDir)
	{
		deleteDir(dd_options, ddp_conn);
		goto cleanup;
	}

	if (dd_options->deleteFile)
	{
		deleteFile(dd_options, ddp_conn);
		goto cleanup;	
	}

	if (dd_options->getFreePercent)
	{
		return getFreePercent(dd_options);
	}

	if (dd_options->readFile)
	{
		failCount = readFileFromDDBoost(dd_options);
		goto cleanup;
	}

	if (dd_options->listDir)
	{
		if(!dd_options->directory)
		{
			mpp_err_msg(logInfo, progname, "Directory not specified for listDir\n");
			goto cleanup;
		}
			
		failCount = listDirectory(dd_options, ddp_conn);
		goto cleanup;
	}

	if(dd_options->table)
	{
		failCount = createFakeRestoreFile(dd_options->table, dd_options, ddp_conn);
		goto cleanup;
	}

	if(dd_options->getLatestTimestamp)
	{
		failCount = getLatestTimestamp(dd_options, ddp_conn, NULL, dd_options->database);
		goto cleanup;
	}

	if(dd_options->syncFiles)
	{
		failCount = syncFilesFromDDBoost(dd_options, ddp_conn, dd_options->directory);
		goto cleanup;
	}

	/* Produce results report */
	failCount = reportRestoreResults(inputOpts.pszReportDirectory, NULL, NULL);

cleanup:
	FreeInputOptions(&inputOpts);
	if (opts != NULL)
		free(opts);

	if(ddp_conn != DDP_INVALID_DESCRIPTOR)
		ddp_disconnect(ddp_conn);

	if(ddp_inst != DDP_INVALID_DESCRIPTOR)
		ddp_instance_destroy(ddp_inst);

	if (dd_options != NULL)
		free(dd_options);	
	
	return (failCount == 0 ? 0 : 1);
}

static void
usage(void)
{
	printf(("%s Utility routine to access DDBoost from Greenplum Database\n\n"), progname);
	printf(("Usage:\n"));
	printf(("  %s [OPTIONS]\n"), progname);

	printf(("\nGeneral options:\n"));
	printf("  --copyFromDDBoost		Copy the backup file from DDBoost to GPDB\n");	
	printf("  --copyToDDBoost		Copy the backup file from GPDB to DDBoost\n");
	printf("  --from-file			The pathname of the source file\n");
	printf("  --to-file			The pathname of the destination file\n");
	printf("  --del-dir=DIR			To delete the specified DIR on DDBoost\n");	
	printf("  --getFreePercent		Get free space percentage on DDBoost\n");
	printf("  --del-file=FILE		To cleanup a specific file on DDboost\n"); 
	printf("  --listDirectory		To list the contents of a directory\n");	
	printf("  --readFile			To read a file on DDBoost\n");	 
	printf("  --dir=DIR			Pathname of directory to list contents or find latest timestamp\n");
	printf("  --database=<Databasename>	Database name \n");	
	printf("  -s				To find the latest timestamp of the dump file\n");	
	printf("  -t				To restore a specific table\n");
	printf("  --sync			To copy over the config and global dump files\n"); 		
	printf(("  --help                   	show this help, then exit\n"));
	printf(("  --version                	output version information, then exit\n"));

	printf(("\nConnection options:\n"));
	printf("  --user			Username to login to DDboost\n");
	printf("  --passwd			Password to login to DDBoost\n");
	printf("  --host			Hostname of DDboost machine\n");
}

bool
fillInputOptions(int argc, char **argv, InputOptions * pInputOpts)
{
	int			c;
	extern int		optind;
	extern char 		*optarg;
	int			lenCmdLineParms;
	int			i;

        char            	*ddboost_user = NULL;
        char            	*ddboost_passwd = NULL;
        char            	*ddboost_hostname = NULL;

	struct option cmdopts[] = {
                {"user", required_argument, NULL, 1},
                {"passwd", required_argument, NULL, 2},
                {"host", required_argument, NULL, 3},
		{"key", required_argument, NULL, 4},
		{"from-file", required_argument, NULL, 5},
		{"to-file", required_argument, NULL, 6},
		{"copyFromDDBoost", no_argument, NULL, 7},
		{"copyToDDBoost", no_argument, NULL, 8},
		{"del-dir", required_argument, NULL, 9},
		{"getFreePercent", no_argument, NULL, 10},
		{"del-file", required_argument, NULL, 11},
		{"readFile", no_argument, NULL, 12},
		{"listDirectory", no_argument, NULL, 13},
		{"t", required_argument, NULL, 14},
		{"getLatestTimestamp", no_argument, NULL, 15},
		{"dir", required_argument, NULL, 16},
		{"database", required_argument, NULL, 17},
		{"sync", no_argument, NULL, 18},
		{NULL, 0, NULL, 0}
	};

	/* Initialize option fields  */
	memset(pInputOpts, 0, sizeof(InputOptions));
	memset(dd_options, 0, sizeof(dd_options));

	opts = (RestoreOptions *) calloc(1, sizeof(RestoreOptions));
	if (opts == NULL)
	{
		mpp_err_msg_cache(logError, progname, "error allocating memory for RestoreOptions");
		return false;
	}

	if (argc == 1)
	{
		usage();
		exit(0);
	}

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("gpddboost (Greenplum Database) " GP_VERSION);
			exit(0);
		}
	}

	/*
	 * Record the command line parms as a string for documentation in the
	 * ouput report
	 */
	lenCmdLineParms = 0;
	for (i = 1; i < argc; i++)
	{
		if (i > 1)
			lenCmdLineParms += 1;

		lenCmdLineParms += strlen(argv[i]);
		if (parmValNeedsQuotes(argv[i]))
			lenCmdLineParms += 2;
	}

	while ((c = getopt_long(argc, argv, "acd:h:ip:RsuU:vwW",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
                        case 1:
                                /* DDBoost user name */
                                ddboost_user = Safe_strdup(optarg);
				dd_options->ddb_username = Safe_strdup(optarg);
                                pInputOpts->pszPassThroughParms = addPassThroughLongParm("gp-ddb-user", ddboost_user, pInputOpts->pszPassThroughParms);
                                dd_boost_enabled = 1;
                                break;
                        case 2:
                                /* DDBoost password */
                                ddboost_passwd = Safe_strdup(optarg);
				dd_options->ddb_password = Safe_strdup(optarg);
                                pInputOpts->pszPassThroughParms = addPassThroughLongParm("gp-ddb-passwd", ddboost_passwd, pInputOpts->pszPassThroughParms);
                                dd_boost_enabled = 1;
                                break;
                        case 3:
                                /* DDBoost hostname */
                                ddboost_hostname = Safe_strdup(optarg);
				dd_options->ddb_hostname = Safe_strdup(optarg);
                                pInputOpts->pszPassThroughParms = addPassThroughLongParm("gp-ddb-hostname", optarg, pInputOpts->pszPassThroughParms);
                                dd_boost_enabled = 1;
                                break;
			case 4:
				/* Timestamp */
				dd_options->timestamp = Safe_strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughLongParm("gp-ddb-key", optarg, pInputOpts->pszPassThroughParms);
				dd_boost_enabled = 1;
				break;
			case 5:
				/* Directory name on gpdb */
				dd_options->from_file = Safe_strdup(optarg);
				break;
			case 6:
				/* Directory name on gpdb */
				dd_options->to_file = Safe_strdup(optarg);
				break;
			case 7:
				/* Cleanup the files on DDBoost */
				dd_options->copyFromDDBoost = true;
				break;
			case 8:
				/* Copy File from DDBoost */
				dd_options->copyToDDBoost = true;
				break;
			case 9:
				/* Cleanup specified directory */
				dd_options->deleteDir = Safe_strdup(optarg);
				break;
			case 10:
				/* Get free percent */
				dd_options->getFreePercent = true;
				break;
			case 11:
				/* Cleanup specified file */
				dd_options->deleteFile = Safe_strdup(optarg);
				break;
			case 12:
				/* read file and write to stdout */
				dd_options->readFile = true;
				break;
			case 13:
				/* List directory contents */
				dd_options->listDir = true;
				break;
			case 14:
				/* Schema.Table to restore */
				if(!optarg)
				{
					mpp_err_msg_cache(logError, progname, "Try \"%s --help\" for more information.\n", progname);
					return false;
				}	
				dd_options->table = Safe_strdup(optarg);
				break;
			case 15:
				dd_options->getLatestTimestamp = true;
				break;
			case 16:
				dd_options->directory = Safe_strdup(optarg);
				if(!optarg)
				{
					mpp_err_msg_cache(logError, progname, "DDBoost directory name missing\n");
					return false;
				}	
				break;
			case 17:
				dd_options->database = Safe_strdup(optarg);
				break;
			case 18:
				dd_options->syncFiles = true;
				break;	
			default:
				mpp_err_msg_cache(logError, progname, "Try \"%s --help\" for more information.\n", progname);
				return false;
		}
	}

	return true;
}

/*
 * parmValNeedsQuotes: This function checks to see whether there is any whitespace in the parameter value.
 * This is used for pass thru parameters, top know whether to enclose them in quotes or not.
 */
bool
parmValNeedsQuotes(const char *Value)
{
	static regex_t rFinder;
	static bool bCompiled = false;
	static bool bAlwaysPutQuotes = false;
	bool		bPutQuotes = false;

	if (!bCompiled)
	{
		if (0 != regcomp(&rFinder, "[[:space:]]", REG_EXTENDED | REG_NOSUB))
			bAlwaysPutQuotes = true;

		bCompiled = true;
	}

	if (!bAlwaysPutQuotes)
	{
		if (regexec(&rFinder, Value, 0, NULL, 0) == 0)
			bPutQuotes = true;
	}
	else
		bPutQuotes = true;

	return bPutQuotes;
}

int
reportRestoreResults(const char *pszReportDirectory, const ThreadParm * pMasterParm, const ThreadParmArray * pParmAr)
{
	return 0;
}

/*
 * the same as reportRestoreResults but only for the master. This is a little ugly
 * since we repeat some of the code but we need a majoy overhaul in dump/restore
 * anyhow - so it will do for now.
 */
int
reportMasterError(InputOptions inputopts, const ThreadParm * pMasterParm, const char *localMsg)
{
	return 0;
}

/*
 * addPassThroughLongParm: this function adds a long option to the string of pass-through parameters.
 * These get sent to each backend and get passed to the gp_dump_agent program.
 */
static char *
addPassThroughLongParm(const char *Parm, const char *pszValue, char *pszPassThroughParmString)
{
        char       *pszRtn;
        bool            bFirstTime = (pszPassThroughParmString == NULL);

        if (pszValue != NULL)
        {
                if (parmValNeedsQuotes(pszValue))
                {
                        PQExpBuffer valueBuf = createPQExpBuffer();

                        if (bFirstTime)
                                pszRtn = MakeString("--%s \"%s\"", Parm, shellEscape(pszValue, valueBuf));
                        else
                                pszRtn = MakeString("%s --%s \"%s\"", pszPassThroughParmString, Parm, shellEscape(pszValue, valueBuf));

                        destroyPQExpBuffer(valueBuf);
                }
                else
                {
                        if (bFirstTime)
                                pszRtn = MakeString("--%s %s", Parm, pszValue);
                        else
                                pszRtn = MakeString("%s --%s %s", pszPassThroughParmString, Parm, pszValue);
                }
        }
        else
        {
                if (bFirstTime)
                        pszRtn = MakeString("--%s", Parm);
                else
                        pszRtn = MakeString("%s --%s", pszPassThroughParmString, Parm);
        }

        return pszRtn;
}

/*
 * shellEscape: Returns a string in which the shell-significant quoted-string characters are
 * escaped.  The resulting string, if used as a SQL statement component, should be quoted
 * using the PG $$ delimiter (or as an E-string with the '\' characters escaped again).
 *
 * This function escapes the following characters: '"', '$', '`', '\', '!'.
 *
 * The PQExpBuffer escapeBuf is used for assembling the escaped string and is reset at the
 * start of this function.
 *
 * The return value of this function is the data area from excapeBuf.
 */
static char *
shellEscape(const char *shellArg, PQExpBuffer escapeBuf)
{
        const char *s = shellArg;
        const char      escape = '\\';

        resetPQExpBuffer(escapeBuf);

        /*
         * Copy the shellArg into the escapeBuf prepending any characters
         * requiring an escape with the escape character.
         */
        while (*s != '\0')
        {
                switch (*s)
                {
                        case '"':
                        case '$':
                        case '\\':
                        case '`':
                        case '!':
                                appendPQExpBufferChar(escapeBuf, escape);
                }
                appendPQExpBufferChar(escapeBuf, *s);
                s++;
        }

        return escapeBuf->data;
}

int
readFromDDFile(FILE *fp, char *ddBoostFileName)
{
        ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
        int err = 0;
        ddp_uint64_t ret_count = 0;
        char *buf = NULL;
        int i = 0;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
        ddp_uint64_t total_bytes = 0;
        size_t written = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
 
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
        path1.path_name = full_path;

        err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
        if (err)
        {
               mpp_err_msg(logInfo, progname,("File open on ddboost failed\n"));
               return -1;
        }

        buf = (char*)malloc(rw_size);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

       do
       {
                memset(buf, 0, rw_size);
                ret_count = 0;
		
		err = ddp_stat(ddp_conn, &path1, &stat_buf);
		if (err)
			break;
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

                err = ddp_read(handle, buf, rw_size,
                rw_size * i, &ret_count);
                if (err)
                        break;
		
                written = fwrite(buf, 1, ret_count, fp);

                total_bytes += ret_count;
                i++;
       } while((err == 0) && (ret_count > 0));

        fclose(fp);
	ddp_close_file(handle);

        return 0;
}

int
initDDSystem(const char *dd_boost_username, const char *dd_boost_passwd, const char *dd_boost_hostname)
{
        int err = DD_ERR_NONE;

        err = ddp_init("gp_dump");
                if (err != DD_ERR_NONE) {
                        mpp_err_msg(logError, progname,"DDInit fails with code %d\n", err);
                        return err;
        }

        err = ddp_instance_create(DDB_POOL_SIZE, &cl_info, &ddp_inst);

        if (err)
        {
                mpp_err_msg(logError, progname,"dd instance creation failed. Err = %d\n", err);
                return err;
        }

        ddp_log_init(ddp_inst, NULL, _ddp_test_log);

        err = ddp_connect_with_user_pwd(ddp_inst, dd_boost_hostname, NULL, dd_boost_username, dd_boost_passwd, &ddp_conn);

        if (err != DD_ERR_NONE) {
                mpp_err_msg(logError, progname, "dd_connect failed. err = %d\n", err);
                return err;
        }

	return 0;
}

void _ddp_test_log(const void *session_ptr, const ddp_char_t *log_msg, ddp_severity_t severity)
{
    FILE* log_file = NULL;
    time_t ltime;
    struct tm *Tm;

    log_file = fopen("libDDBoost_test.log", "a");
    if (log_file) {
        ltime = time(NULL);
        Tm = localtime(&ltime);
        fprintf(log_file, "%02d/%02d %02d:%02d:%02d: %s\n",
                           Tm->tm_mon + 1, Tm->tm_mday,
                           Tm->tm_hour + (1 - Tm->tm_isdst), Tm->tm_min, Tm->tm_sec,
                           log_msg);
        fclose(log_file);
    }
}

int 
copyFileFromDDBoost(struct ddboost_options *dd_options)
{
	char *ddboostFile = strdup(dd_options->from_file);
	char *gpdbFile = strdup(dd_options->to_file);
	FILE *fp = NULL;

	if (!ddboostFile)
	{
		mpp_err_msg(logError, progname, "Source File on DDboost not specified\n");
		return -1;
	}

	if (!gpdbFile)
	{
		mpp_err_msg(logError, progname, "Destination file on GPDB not specified\n");
		return -1;
	}

	fp = fopen(gpdbFile, "w");
	if (fp == NULL)
	{
		mpp_err_msg(logError, progname, "Cannot open file %s on GPDB\n", gpdbFile);
		return -1;
	}

	return readFromDDFile(fp, ddboostFile);	

}

int 
copyFileToDDBoost(struct ddboost_options *dd_options)
{
	char *gpdbFile = strdup(dd_options->from_file);
	char *ddBoostFile = strdup(dd_options->to_file);
	FILE *fp = NULL;

	if (!ddBoostFile)
	{
		mpp_err_msg(logError, progname, "Destination file on DDboost not specified\n");
		return -1;
	}

	if (!gpdbFile)
	{
		mpp_err_msg(logError, progname, "Source file on GPDB not specified\n");
		return -1;
	}

	fp = fopen(gpdbFile, "r");
	if (fp == NULL)
	{
		mpp_err_msg(logError, progname, "Cannot open file %s on GPDB\n", gpdbFile);
		return -1;
	}

	return writeToDDFile(fp, ddBoostFile);

}

int 
readFileFromDDBoost(struct ddboost_options *dd_options)
{
	char *ddBoostFile = strdup(dd_options->from_file);

	if (!ddBoostFile)
	{
		mpp_err_msg(logError, progname, "Destination file on DDboost not specified\n");
		return -1;
	}

	return readFromDDFileToOutput(ddBoostFile);

}

int 
deleteDir(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = strdup(dd_options->deleteDir);
        int err = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
 
	if (!ddboostDir)
	{
		mpp_err_msg(logInfo, progname, "Directory on DDboost is not specified\n");
		return -1;
	}	
        
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", ddboostDir);

	path1.su_name = storage_unit_name;
	path1.path_name = ddboostDir;

	err = ddBoostRmdir(ddboostDir, ddp_conn, ddboostDir);
	if (err)
	{
		mpp_err_msg(logInfo, progname, "Deleting Directory on GPDB failed with err %d\n", err);
		return -1;
	}

	return err;
}

int 
ddBoostRmdir(char *dir_path, ddp_conn_desc_t ddp_conn, char *parent_dir)
{
	ddp_path_t temp_path = {0};
	ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    	dd_errno_t err = DD_ERR_NONE;
    	ddp_dirent_t ret_dirent;
	char *full_path = NULL;
	char *cur_path = NULL;

	if (!dir_path)
		return 0;

	temp_path.path_name = dir_path;
	temp_path.su_name = "GPDB";

	err = ddp_open_dir(ddp_conn, &temp_path, &dird);
	if (err)
	{
		mpp_err_msg(logInfo, progname, "Opening directory %s on ddboost failed\n", dir_path);
		return -1;
	}

	cur_path = (char*)malloc(MAX_PATH_NAME);
	if (!cur_path)
	{
		mpp_err_msg(logInfo, progname, "Memory allocation failure\n");
		return -1;
	}
	memset(cur_path, 0, MAX_PATH_NAME);	

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (!full_path)
	{
		mpp_err_msg(logInfo, progname, "Memory allocation failure\n");
		return -1;
	}
	memset(full_path, 0 , MAX_PATH_NAME);

	strcat(full_path, parent_dir);
	if (strcmp(&(full_path[strlen(full_path) - 1]), "/"))
		strcat(full_path, "/");

	/* Try to remove the directory. If the err is DD_ERR_NOTEMPTY then recursively delete the contents */
	err = ddp_rmdir(ddp_conn, &temp_path);
	if (err && err != DD_ERR_NOTEMPTY)
	{
		mpp_err_msg(logInfo, progname, "Directory deletion failed with err %d\n", err);
		return -1;
	}

	while (1) 
	{
        	memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
        	err = ddp_readdir(dird, &ret_dirent);
        	if (err != DD_OK) 
		{
            		if (err == DD_ERR_UNDERFLOW) 
			{
                		err = DD_OK;
                		break;
            		} 
			else 
			{
                		return err;	
            		}
        	}

		else 
		{
			memset(cur_path, 0, MAX_PATH_NAME);
			strcat(cur_path, full_path);
			strcat(cur_path, ret_dirent.d_name);

			/* Skip deleting . and .. */
			if (strcmp("..", ret_dirent.d_name) == 0)
			{
				continue;
			}
			if (strcmp(".", ret_dirent.d_name) == 0)
			{
				continue;
			}

			/* Try to unlink the file. If the err is FILE_IS_DIR then call the same function */
			temp_path.path_name = cur_path;
			err = ddp_unlink(ddp_conn, &temp_path);		

			if (err && (err == DD_ERR_FILE_IS_DIR))
			{
				err = ddBoostRmdir(cur_path, ddp_conn, cur_path);
				if (!err)
				{
					/* Successfully deleted directory */
					continue;
				}
				else
				{
					/* Error in deleting directory */
					return err;
				}
					
			}	
			else if (err)
			{
				/* Deleting file failed with error */
				return err;
			}
			else
			{
				/* File succesfully deletd */
			}
			
        	}
    	}

	temp_path.path_name = dir_path;
	err = ddp_rmdir(ddp_conn, &temp_path);
        if (err)
        {
                /* Directory deletion failed with err */
                return -1;
        }
	else
		/* Successfully deleted directory */	
	return 0;
}

static int writeToDDFile(FILE *fp, char *ddBoostFileName)	
{
	ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
        int err = 0;
        ddp_uint64_t ret_count = 0;
        char *buf = NULL;
        int i = 0;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
        ddp_uint64_t total_bytes = 0;
	ddp_uint64_t read_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
 
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
        path1.path_name = full_path;

	err = createDDboostDir(ddp_conn, storage_unit_name, ddBoostFileName);
	if (err)
	{
		mpp_err_msg(logInfo, progname, "Creating directory on ddboost failed. Err %d\n", err);
		return -1;
	}

        err = ddp_open_file(ddp_conn, &path1, DDP_O_CREAT | DDP_O_RDWR, 0600, &handle);
        if (err)
        {
               mpp_err_msg(logInfo, progname,"File open on ddboost failed. Err %d\n", err);
               return -1;
        }

        buf = (char*)malloc(rw_size);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

       	do
       	{
                memset(buf, 0, rw_size);
                ret_count = 0;

                read_bytes = fread(buf, 1, rw_size, fp);

		err = ddp_write(handle, buf, read_bytes,
                rw_size * i, &ret_count);

                if (err)
                        break;

                total_bytes += ret_count;
                i++;
       	} while((err == 0) && (ret_count > 0) && (rw_size == read_bytes));

	err = ddp_stat(ddp_conn, &path1, &stat_buf);
		
	if (stat_buf.st_size < rw_size )
		rw_size = stat_buf.st_size;	

        fclose(fp);
	ddp_close_file(handle);

        return 0;
}

float getFreePercent(struct ddboost_options *dd_options)
{
	unsigned long long total_bytes = 0;
	unsigned long long free_bytes = 0;
	double percent = 0.0;	
	ddp_statvfs_t statfs_buf;
	int err = 0;
	 
	err = ddp_statfs(ddp_conn, &statfs_buf);
	if (err)
		return 0.0;
	else
	{
		total_bytes = statfs_buf.f_blocks;
		free_bytes = statfs_buf.f_bfree;		
	}

	percent = 100.0 * (double)free_bytes/(double)total_bytes;

	return percent;
}

int 
deleteFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	ddp_path_t temp_path = {0};
    	dd_errno_t err = DD_ERR_NONE;
	char *temp = NULL;

	if (!dd_options->deleteFile)
		return 0;

	temp = (char*)malloc(MAX_PATH_NAME);
	if (!temp)
	{
		printf("\nMemory allocation failure\n");
		return -1;
	}

	memset(temp, 0, MAX_PATH_NAME);

	strcat(temp, dd_options->deleteFile);

	temp_path.path_name = temp;
	temp_path.su_name = "GPDB";
	
	err = ddp_unlink(ddp_conn, &temp_path);		
	if (err)
	{
		printf("\nFile %s cannot be deleted on DDboost. Error %d\n", temp_path.path_name, err);
		free(temp);
		return err;
	}

	free(temp);
	return 0;
}

int
readFromDDFileToOutput(char *ddBoostFileName)
{
        ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
        int err = 0;
        ddp_uint64_t ret_count = 0;
        char *buf = NULL;
        int i = 0;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
        ddp_uint64_t total_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
        path1.path_name = full_path;

        err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
        if (err)
        {
               mpp_err_msg(logInfo, progname,("File open on ddboost failed\n"));
               return -1;
        }

        buf = (char*)malloc(rw_size);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

       do
       {
                memset(buf, 0, rw_size);
                ret_count = 0;
		
		err = ddp_stat(ddp_conn, &path1, &stat_buf);
		if (err)
			break;
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

                err = ddp_read(handle, buf, rw_size,
                rw_size * i, &ret_count);
                if (err)
                        break;
	
		write(1, buf, ret_count);

                total_bytes += ret_count;
                i++;
       } while((err == 0) && (ret_count > 0));

        return 0;
}

int 
createDDboostDir(ddp_conn_desc_t ddp_conn, char *storage_unit_name, const char *filePath)
{
	char *pch = NULL;
        ddp_path_t path = {0};
        int err = 0;
        char *full_path = NULL;
	char *path_name = strdup(filePath);

        full_path = (char*)malloc(MAX_PATH_NAME);
        memset(full_path, 0, MAX_PATH_NAME);

        pch = strtok(path_name, " /");
        while(pch != NULL)
        {
                path.su_name = storage_unit_name;
                strcat(full_path, pch);
                path.path_name = full_path;

		if (strcmp(full_path, filePath))
		{
                	err = ddp_mkdir(ddp_conn, &path, 0755);
                	if ((err != 0) && (err != DD_ERR_EXIST))
                	{
                        	mpp_err_msg(logError, progname, "mkdir failed. err %d\n", err);
                        	return err;
                	}

                	strcat(full_path, "/");
                	pch = strtok(NULL, " /");
		}
		else
			break;
        }
        return 0;
}

int listDirectory(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = strdup(dd_options->directory);
        int err = 0;
        ddp_path_t path1 = {0};
        char *storage_unit_name = NULL;
        char *full_path = NULL;
        ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
        ddp_dirent_t ret_dirent;

        if (!ddboostDir)
        {
                mpp_err_msg(logInfo, progname, "Directory on DDboost is not specified\n");
                return -1;
        }

        storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

        full_path = (char*)malloc(MAX_PATH_NAME);
        sprintf(full_path, "%s", ddboostDir);

        path1.su_name = storage_unit_name;
        path1.path_name = ddboostDir;

	err = ddp_open_dir(ddp_conn, &path1, &dird);
        if (err)
        {
                mpp_err_msg(logInfo, progname, "Opening directory %s on ddboost failed\n", ddboostDir);
                return -1;
        }

	while (1)
        {
                memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
                err = ddp_readdir(dird, &ret_dirent);
                if (err != DD_OK)
                {
                        if (err == DD_ERR_UNDERFLOW)
                        {
                                err = DD_OK;
                                break;
                        }
                        else
                        {
                                return err;
                        }
                }

                else
                {
                        /* Skip deleting . and .. */
                        if (strcmp("..", ret_dirent.d_name) == 0)
                        {
                                continue;
                        }
                        if (strcmp(".", ret_dirent.d_name) == 0)
                        {
                                continue;
                        }

			printf("\n%s", ret_dirent.d_name);
		}
	}
	printf("\n");
	return err;
}

static int readFromLocalFileToOutput(char *fileName)
{
        char *buf = NULL;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
        ddp_uint64_t ret_count = 0;
	FILE *fp = NULL;

        fp = fopen(fileName, "r");
        if (fp == NULL)
        {
               mpp_err_msg(logInfo, progname,("File open on GPDB failed\n"));
               return -1;
        }

        buf = (char*)malloc(rw_size);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

      	 do
       	{
                memset(buf, 0, rw_size);
                ret_count = 0;
	
                ret_count = fread(buf, 1, rw_size, fp);
                if (ret_count <= 0)
                        break;

		write(1, buf, ret_count);

       	} while(ret_count > 0);

        return 0;
}

/* For selective restore we need to parse the specified dump file
 * for psql statements corresponding to the required table 
 */
static int 
createFakeRestoreFile(char *schemaTable, struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
        ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
        int err = 0;
        ddp_uint64_t ret_count = 0;
        char *buf = NULL;
        int i = 0;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
        ddp_uint64_t total_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
	int fd[2];
	char line[101];
	FILE *fp = NULL, *fpTemp = NULL;
	int done = false;
	char *schema = NULL;
	char *table = NULL;
	char *searchString = NULL;
	char *endString = NULL;
	char *copyString = NULL;
	int output, in_schema, found;

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", dd_options->from_file);

	path1.su_name = storage_unit_name;
        path1.path_name = full_path;

        err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
        if (err)
        {
               mpp_err_msg(logInfo, progname,("File open on ddboost failed\n"));
               return -1;
        }

        buf = (char*)malloc(rw_size);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

	/* Get the schema and table name from the string */
	splitSchemaAndTableName(schemaTable, &schema, &table);

	/* Create two processes. In one write to named pipe.
  	 * In the other read from the named pipe and use getline to
	 * parse as before.
	 * Close the pipe after the file is exhausted */	

	pipe(fd);	
	
	if (fork() == 0)
	{	
		/* Child reads from DDboost and writes it to pipe */
		close(fd[0]);	
       		do
       		{
                	memset(buf, 0, rw_size);
                	ret_count = 0;
		
			err = ddp_stat(ddp_conn, &path1, &stat_buf);
			if (err)
				break;
		
			if (stat_buf.st_size < rw_size )
				rw_size = stat_buf.st_size;	

                	err = ddp_read(handle, buf, rw_size,
                	rw_size * i, &ret_count);
                	if (err)
                        	break;
	
			write(fd[1], buf, ret_count);

                	total_bytes += ret_count;
                	i++;
       		} while ((err == 0) && (ret_count > 0));
		done = 1;
		close(fd[1]);
	}
	else
	{
		/* Parent reader */
		close(fd[1]);
		fp = fdopen(fd[0], "r");
		if (fp == NULL)
		{
			printf("\nOpening File stream failed\n");
			return 1;
		}

		if(!dd_options->to_file)
		{
			printf("\nOutput file not specified\n");
			return -1;
		}

		fpTemp = fopen(dd_options->to_file, "w");
		if(!fpTemp)
		{
			printf("\nFile open on GPDB failed\n");
			return -1;
		}

		searchString = (char*)malloc(MAX_PATH_NAME);
		copyString = (char*)malloc(MAX_PATH_NAME);
		endString = (char*)malloc(MAX_PATH_NAME);
	
		memset(searchString, 0, MAX_PATH_NAME);
		memset(copyString, 0, MAX_PATH_NAME);	
		memset(endString, 0, MAX_PATH_NAME);
	
		sprintf(searchString, "SET search_path = %s", schema);
		sprintf(copyString, "COPY %s", table);
		sprintf(endString, "\\.");

		output = false;
		in_schema = false;
		found = false;
	
	
		do
		{
			memset(line, 0, 101);
			if (fgets(line, 100, fp) != NULL)
			{
				if (lineStartsWith(line, searchString))
				{	
					fwrite(line, 1, strlen(line), fpTemp);
					in_schema = true;
				}
				else if (in_schema && lineStartsWith(line, copyString))
				{
					found = true;
					output = true;
				}
				else if (lineEndsWith(line, endString))
				{
					fwrite(line, 1, strlen(line), fpTemp);
					output = false;
					in_schema = false;
				}
				
				if(output && in_schema)
				{
					fwrite(line, 1, strlen(line), fpTemp);
				}
				
			}
			else
				done = true;

		} while (!done);
		close(fd[0]);
	}
	
	return 0;
}

/* check if the line starts with the specified pattern */
int 
lineStartsWith(char *line, char *pattern)
{
	int i = 0;
	
	/* Skip spaces at the beginning */	
	while(line[i] == ' ')
		i++;

	if(strncmp(&line[i], pattern, strlen(pattern)))
		return 0;
	else
		return 1;
} 

/* Check if the line ends with the specified pattern */	
int 
lineEndsWith(char *line, char *pattern)
{
	if(strstr(line, pattern))
		return 1;
	else
		return 0;

}
	 
int 
splitSchemaAndTableName(char *schemaTable, char **schema, char **table)
{
	char *pch = NULL;
	pch = strtok(schemaTable, ". ");
	*schema = Safe_strdup(pch);

	pch = strtok(NULL, ". ");
	*table = Safe_strdup(pch);

	return 0;
}

static int 
getLatestTimestamp(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath, char *databaseName)
{
	
	char *ddboostDir = strdup(dd_options->directory);
        int err = 0;
        ddp_path_t path1 = {0};
        char *storage_unit_name = NULL;
        char *full_path = NULL;
        ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
        ddp_dirent_t ret_dirent;
	char *recentTimestamp = NULL;
	char *filePath = NULL;
	int found  = 0;

        if (!ddboostDir)
        {
                mpp_err_msg(logInfo, progname, "Directory on DDboost is not specified\n");
                return -1;
        }

        storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

        full_path = (char*)malloc(MAX_PATH_NAME);
        sprintf(full_path, "%s", ddboostDir);

        path1.su_name = storage_unit_name;
        path1.path_name = ddboostDir;

	err = ddp_open_dir(ddp_conn, &path1, &dird);
        if (err)
        {
                mpp_err_msg(logInfo, progname, "Directory %s doesn't exist on ddboost\n", ddboostDir);
                return -1;
        }

	recentTimestamp = (char*)malloc(MAX_PATH_NAME);
	if(!recentTimestamp)
	{
		printf("\nMemory allocation failed in gpdboost\n");
		return -1;
	}	
	memset(recentTimestamp, 0, MAX_PATH_NAME);

	filePath = (char*)malloc(MAX_PATH_NAME);
	if(!filePath)
	{
		printf("\nMemory allocation failed in gpddboost\n");
		return -1;
	}
	memset(filePath, 0, MAX_PATH_NAME);

	while (1)
        {
                memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
                err = ddp_readdir(dird, &ret_dirent);
                if (err != DD_OK)
                {
                        if (err == DD_ERR_UNDERFLOW)
                        {
                                err = DD_OK;
                                break;
                        }
                        else
                        {
                                return err;
                        }
                }

                else
                {
                        /* Skip deleting . and .. */
                        if (strcmp("..", ret_dirent.d_name) == 0)
                        {
                                continue;
                        }
                        if (strcmp(".", ret_dirent.d_name) == 0)
                        {
                                continue;
                        }

			memset(filePath, 0, MAX_PATH_NAME);
			sprintf(filePath, "%s/%s", ddboostDir, ret_dirent.d_name);

			/* Compare only cdatabase files	*/
			if(strncmp(ret_dirent.d_name, "gp_cdatabase", strlen("gp_cdatabase")))
			{
				continue;
			}		

			/* If database name doesn't match then continue */
			if (!dumpFileHasDatabaseName(dd_options, ddp_conn, filePath, databaseName))
				continue;

			found  = 1;

			if (strcmp(ret_dirent.d_name, recentTimestamp) > 0)
			{
				memset(recentTimestamp, 0, MAX_PATH_NAME);
				strncpy(recentTimestamp, ret_dirent.d_name, strlen(ret_dirent.d_name)+1);
			}	
		}
	}
	
	if(found)
		printf("\n%s\n", &recentTimestamp[strlen(recentTimestamp)-14]);
	else
		printf("\nNo dump file with the specified database exists\n");
	return err;
}


int
dumpFileHasDatabaseName(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *fullPath, char *databaseName)
{
	/* Read the first 5 lines of the cdatabase file on DDboost */
        ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
        int err = 0;
        ddp_uint64_t ret_count = 0;
        char *buf = NULL;
        ddp_uint64_t rw_size = DDB_IO_SIZE;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
	char *searchString = NULL;

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", fullPath);

	path1.su_name = storage_unit_name;
        path1.path_name = full_path;

        err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
        if (err)
        {
	       printf("\nFile path is %s\n", full_path);	
               mpp_err_msg(logInfo, progname,("File open on ddboost failed\n"));
               return -1;
        }

        buf = (char*)malloc(MAX_PATH_NAME + 1);
        if (buf == NULL)
        {
               mpp_err_msg(logInfo, progname, "Memory allocation failed\n");
               return -2;
        }

	/* The CREATE DATABASE statement is usually in the first 5 lines */
        memset(buf, 0, MAX_PATH_NAME + 1);
        ret_count = 0;
		
	err = ddp_stat(ddp_conn, &path1, &stat_buf);
	if (err)
		return 0;	
		
	if (stat_buf.st_size < MAX_PATH_NAME )
		rw_size = stat_buf.st_size;	

        err = ddp_read(handle, buf, MAX_PATH_NAME,
              	0, &ret_count);

        if (err)
                return 0;
	
	searchString = (char*)malloc(MAX_PATH_NAME);
	memset(searchString, 0, MAX_PATH_NAME);
	sprintf(searchString, "CREATE DATABASE %s", databaseName);

	if(strstr(buf, searchString))
		return 1;

	return 0;
}

/* Copy over files from DDboost to GPDB master segment directory */
int 
syncFilesFromDDBoost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath)
{
	char *ddboostDir = "db_dumps";
        int err = 0;
        ddp_path_t path1 = {0};
        char *storage_unit_name = NULL;
        char *full_path = NULL;
	char *dest_path = NULL;
        ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
        ddp_dirent_t ret_dirent;

        if (!ddboostDir)
        {
                mpp_err_msg(logInfo, progname, "Directory on DDboost is not specified\n");
                return -1;
        }

        storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

        full_path = (char*)malloc(MAX_PATH_NAME);
        sprintf(full_path, "%s", ddboostDir);

        path1.su_name = storage_unit_name;
        path1.path_name = ddboostDir;

	/* Start traversing from db_dumps/ directory */
	err = ddp_open_dir(ddp_conn, &path1, &dird);
        if (err)
        {
                mpp_err_msg(logInfo, progname, "Opening directory %s on ddboost failed\n", ddboostDir);
                return -1;
        }

	/* Create the db_dumps directory if it doesn't exist already */
	dest_path = (char*)malloc(MAX_PATH_NAME);
	memset(dest_path, 0, MAX_PATH_NAME);
	sprintf(dest_path, "%s/%s", dirPath, ddboostDir); 
	
	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if(err && (err != EEXIST) && (err != -1))
	{
		printf("\nDirectory %s creation on GPDB path failed with error %d\n", dest_path, err);
		return -1;
	}

	while (1)
        {
                memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
                err = ddp_readdir(dird, &ret_dirent);
                if (err != DD_OK)
                {
                        if (err == DD_ERR_UNDERFLOW)
                        {
                                err = DD_OK;
                                break;
                        }
                        else
                        {
                                return err;
                        }
                }

                else
                {
                        if (strcmp("..", ret_dirent.d_name) == 0)
                                continue;
                        
			if (strcmp(".", ret_dirent.d_name) == 0)
                                continue;

			/* Only check directories of the form YYYYMMDD */
			if(!isdigit(ret_dirent.d_name[0]))
				continue;

	 		/* For every date, copy the required files over to GPDB */
			memset(full_path, 0, MAX_PATH_NAME);
			sprintf(full_path, "%s/%s", ddboostDir, ret_dirent.d_name);

			copyFilesFromDir(full_path, dirPath, ddp_conn);
		}
	}
	return err;
}


int 
copyFilesFromDir(const char *fromDir, char *toDir, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = Safe_strdup(fromDir);
        int err = 0;
        ddp_path_t path1 = {0};
        char *storage_unit_name = NULL;
        char *full_path = NULL;
        ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
        ddp_dirent_t ret_dirent;
	char *dest_path = NULL;
	FILE *fp = NULL;

        if (!ddboostDir)
        {
                mpp_err_msg(logInfo, progname, "Directory on DDboost is not specified\n");
                return -1;
        }

        storage_unit_name = (char*)malloc(MAX_PATH_NAME);
        sprintf(storage_unit_name, "%s", "GPDB");

        full_path = (char*)malloc(MAX_PATH_NAME);
	memset(full_path, 0, MAX_PATH_NAME);
        sprintf(full_path, "%s", ddboostDir);

	dest_path = (char*)malloc(MAX_PATH_NAME);
	memset(dest_path, 0, MAX_PATH_NAME);
	sprintf(dest_path, "%s/%s", toDir, fromDir); 

	path1.su_name = storage_unit_name;
        path1.path_name = ddboostDir;

	/* Start traversing from db_dumps/ directory */
	err = ddp_open_dir(ddp_conn, &path1, &dird);
        if (err)
        {
                mpp_err_msg(logInfo, progname, "Opening directory %s on ddboost failed\n", ddboostDir);
                return -1;
        }

	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if(err && (err != EEXIST) && (err != -1))
	{
		printf("\nDirectory %s creation on GPDB path failed with error %d\n", dest_path, err);
		return -1;
	}

	while (1)
        {
                memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
                err = ddp_readdir(dird, &ret_dirent);
                if (err != DD_OK)
                {
                        if (err == DD_ERR_UNDERFLOW)
                        {
                                err = DD_OK;
                                break;
                        }
                        else
                        {
                                return err;
                        }
                }

                else
                {
                        if (strcmp("..", ret_dirent.d_name) == 0)
                                continue;
                        
			if (strcmp(".", ret_dirent.d_name) == 0)
                                continue;

			if(!isFileToBeCopied(ret_dirent.d_name))
			{
				continue;
			}

	 		/* For every date, copy the required files over to GPDB */
			memset(full_path, 0, MAX_PATH_NAME);
			sprintf(full_path, "%s/%s", ddboostDir, ret_dirent.d_name);

			memset(dest_path, 0, MAX_PATH_NAME);
			sprintf(dest_path, "%s/%s", toDir, full_path);

			fp = fopen(dest_path, "w");
			if(!fp)
			{
				printf("\nFile %s open failed on GPDB\n", dest_path);
				return -1;
			}

			readFromDDFile(fp, full_path);
		}
	}
	return 0;
}

int 
isFileToBeCopied(const char *filename)
{
	if(!strncmp(filename, "gp_global_1_1_", strlen("gp_global_1_1_")))
		return 1;

	if(!strncmp(filename, "gp_cdatabase_1_1_", strlen("gp_cdatabase_1_1_")))
		return 1;

	if(strstr(filename, "_post_data"))
		return 1;

	if(strstr(filename, ".tar"))
		return 1;

	return 0;
}

#else
int main(int argc, char **argv)
{
	printf("\ngpddboost not supported on this platform\n");	
	return 0;
}	
#endif
