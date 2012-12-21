/*-------------------------------------------------------------------------
 *
 * gpddboost.c
 *	  gpddboost is a utility for accessing Data Domain's ddboost 
 *        interfaces.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "postgres_fe.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"       /* Builtin functions, including lower() */
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <regex.h>
#include <ctype.h>
#include <pthread.h>
#include "getopt_long.h"
#include "pqexpbuffer.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include "libpq-fe.h"
#include "fe-auth.h"
#include "pg_backup.h"
#include "cdb_table.h"
#include "cdb_dump_util.h"
#include "cdb_backup_state.h"
#include "cdb_dump.h"
#include <sys/stat.h>
#include <sys/types.h>

/*
 * Note: zlib.h must be included *after* libpq-fe.h, because the latter may
 * include ssl.h, which has a naming conflict with zlib.h.
 */
#include <zlib.h>
#define GZCLOSE(fh) gzclose(fh)
#define GZWRITE(p, s, n, fh) gzwrite(fh, p, (n)*(s))
#define GZGETS(p, n, fh) gzgets(fh, p, n)
#define GZOPEN(path, mode) gzopen(path, mode)
#define GZDOPEN(path, mode) gzdopen(path, mode)

#define DDWRITE(p, s, n, fh, compress) (((compress) == 1)? (GZWRITE(p, s, n, fh)) : (fwrite(p, s, n, fh)))
#define DDCLOSE(fh, compress) (((compress) == 1) ? (GZCLOSE(fh)) : (fclose(fh)))
#define DDGETS(p, n, fh, compress) (((compress) == 1) ? (GZGETS(p, n, fh)) : (fgets(p, n, fh)))
#define DDOPEN(path, mode, compress) (((compress) == 1) ? (GZOPEN(path, mode)) : (fopen(path, mode)))
#define DDDOPEN(path, mode, compress) (((compress) == 1) ? (GZDOPEN(path, mode)) : (fdopen(path, mode)))

#ifdef USE_DDBOOST
#include "ddp_api.h"

#define MAX_PATH_NAME 1024
#define DDB_POOL_SIZE (32 * 1024 * 2048)

/* This is necessary on platforms where optreset variable is not available.
 * Look at "man getopt" documentation for details.
 */
#ifndef HAVE_OPTRESET
int			optreset;
#endif

static ddp_inst_desc_t ddp_inst = DDP_INVALID_DESCRIPTOR;
static ddp_conn_desc_t ddp_conn = DDP_INVALID_DESCRIPTOR;
static char *DDP_SU_NAME = NULL;
static int dd_boost_buf_size = 512*1024;

struct schemaTableList
{
	char *tableName;
	char *schemaName;
	struct schemaTableList *next;
};

struct ddboost_options
{
	char *timestamp;
	char *directory;	
	char *from_file;
	char *to_file;
	char *deleteDir;
	char *deleteFile;
	struct schemaTableList *tableList;
	char *database;
	bool listDir;
	bool copyFromDDBoost;	
	bool copyToDDBoost;
	bool readFile;	
	bool getFreePercent;
	bool getLatestTimestamp;
	bool syncFiles;
	bool writeToDDFileFromInput;
};

static struct ddboost_options *dd_options = NULL;

/* Forward decls */
static bool fillInputOptions(int argc, char **argv, InputOptions * pInputOpts);
static bool parmValNeedsQuotes(const char *Value);
static void usage(void);
static int readFromDDFile(FILE *fp, char *ddBoostFileName);	
static int readFromDDFileToOutput(char *ddBoostFileName);
static int writeToDDFileFromInput(char *ddBoostFileName);
static int writeToDDFile(FILE *fp, char *ddBoostFileName);	

typedef struct option optType;

static RestoreOptions *opts = NULL;

static const char *logError = "ERROR";
const char *progname;

static char * addPassThroughLongParm(const char *Parm, const char *pszValue, char *pszPassThroughParmString);
static char * shellEscape(const char *shellArg, PQExpBuffer escapeBuf);
static int dd_boost_enabled = 1;

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
static int createFakeRestoreFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn); 
static int splitSchemaAndTableName(char *schemaTable, char **schema, char **table);
static int lineStartsWithSchema(char *line, struct schemaTableList *tableList, char **curSchema);
static int lineStartsWithTable(char *line, struct schemaTableList *tableList, char *curSchema);
static int lineEndsWith(char *line, char *pattern);
static int getLatestTimestamp(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath, char *database);
static int dumpFileHasDatabaseName(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath, char *database);
static int syncFilesFromDDBoost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath);
static int isFileToBeCopied(const char*);
static int copyFilesFromDir(const char *fromDir, char *toDir, ddp_conn_desc_t ddp_conn);
static struct schemaTableList *getNode(char *tableName);
static void insertTableName(char *table, struct ddboost_options **dd_options);
static void deleteTableName(char *table, char *schema, struct ddboost_options **dd_options);
static void cleanupTableList(struct ddboost_options **dd_options);
static int isFileCompressed(const char *full_path);
static int createDbdumpsDir(char *filePath);

/* Helper routines for handling multiple -T options using a linked list */
struct schemaTableList *getNode(char *schemaTable)
{
	char *schema = NULL, *table = NULL;

    struct schemaTableList *temp = (struct schemaTableList*)malloc(sizeof(struct schemaTableList));
    if (!temp)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        return NULL;
    }

    temp->next = NULL;
    /* Get the schema and table name from the string */
    splitSchemaAndTableName(schemaTable, &schema, &table);
    temp->tableName = strdup(table);
    temp->schemaName = strdup(schema);

    free(schema);
    free(table);

    return temp;
}

void insertTableName(char *schemaTable, struct ddboost_options **dd_options)
{
    struct schemaTableList *temp = NULL;

    temp = getNode(schemaTable);
    temp->next = (*dd_options)->tableList;
    (*dd_options)->tableList = temp;
}

void deleteTableName(char *table, char *schema, struct ddboost_options **dd_options)
{
    struct schemaTableList *prev = NULL, *cur = (*dd_options)->tableList;

    while (cur != NULL)
    {
        prev = cur;
        cur = cur->next;

        if ((!strcmp(table, prev->tableName)) && (!strcmp(schema, prev->schemaName)))
        {
            if (prev == (*dd_options)->tableList)
                (*dd_options)->tableList = ((*dd_options)->tableList)->next;

            free(prev->tableName);
            free(prev->schemaName);
            free(prev);
        }
    }
}

void cleanupTableList(struct ddboost_options **dd_options)
{
	struct schemaTableList *prev = (*dd_options)->tableList, *cur = NULL;

	while (prev != NULL)
	{
		cur = prev->next;
		deleteTableName(prev->tableName, prev->schemaName, dd_options);
		prev = cur;
	}
}
	
int
main(int argc, char **argv)
{
	int			failCount = 0;
	int			ret = 0;

	/* This struct holds the values of the command line parameters */
	InputOptions inputOpts;

	dd_options = (struct ddboost_options*)malloc(sizeof(struct ddboost_options));
	memset(dd_options, 0, sizeof(struct ddboost_options));

	progname = get_progname(argv[0]);

	/* Parse command line for options */
	if (!fillInputOptions(argc, argv, &inputOpts))
	{
		failCount = 1;
		mpp_err_msg(logError, progname, "Error in InputOptions.\n");
		goto cleanup;
	}

	ret = initDDSystem(&ddp_inst, &ddp_conn, &cl_info, &DDP_SU_NAME);
	
	if (ret)
	{
		mpp_err_msg(logError, progname, "Initialization of ddboost failed. Check login credentials\n");
		goto cleanup;
	}

	/* Take appropriate actions here */
	if (dd_options->copyFromDDBoost)
	{
		failCount = copyFileFromDDBoost(dd_options);
		goto cleanup;
	}

	if (dd_options->copyToDDBoost)
	{
		failCount = copyFileToDDBoost(dd_options);
		goto cleanup;
	}

	if (dd_options->deleteDir)
	{
		failCount = deleteDir(dd_options, ddp_conn);
		goto cleanup;
	}

	if (dd_options->deleteFile)
	{
		failCount = deleteFile(dd_options, ddp_conn);
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
		if (!dd_options->directory)
		{
			mpp_err_msg(logError, progname, "Directory not specified for listDir\n");
			goto cleanup;
		}
			
		failCount = listDirectory(dd_options, ddp_conn);
		goto cleanup;
	}

	if (dd_options->tableList)
	{
		failCount = createFakeRestoreFile(dd_options, ddp_conn);
		cleanupTableList(&dd_options);
		goto cleanup;
	}

	if (dd_options->getLatestTimestamp)
	{
		failCount = getLatestTimestamp(dd_options, ddp_conn, NULL, dd_options->database);
		goto cleanup;
	}

	if (dd_options->syncFiles)
	{
		failCount = syncFilesFromDDBoost(dd_options, ddp_conn, dd_options->directory);
		goto cleanup;
	}

	if (dd_options->writeToDDFileFromInput)
	{
		failCount = writeToDDFileFromInput(dd_options->to_file);
		goto cleanup;
	}

cleanup:
	FreeInputOptions(&inputOpts);
	if (opts != NULL)
		free(opts);

	if (ddp_conn != DDP_INVALID_DESCRIPTOR)
		ddp_disconnect(ddp_conn);

	if (ddp_inst != DDP_INVALID_DESCRIPTOR)
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
	printf("  --write-file-from-stdin 	To write a file to DDBoost from stdin\n");
	printf("  --dir=DIR			Pathname of directory to list contents or find latest timestamp\n");
	printf("  --database=<Databasename>	Database name \n");	
	printf("  --table				To restore a specific table\n");
	printf("  --sync			To copy over the config and global dump files\n"); 		
	printf("  --dd_boost_buf_size		DDboost I/O buffer size\n");
	printf(("  --help                   	show this help, then exit\n"));
	printf(("  --version                	output version information, then exit\n"));
}

bool
fillInputOptions(int argc, char **argv, InputOptions * pInputOpts)
{
	int			c;
	extern int		optind;
	extern char 		*optarg;
	int			lenCmdLineParms;
	int			i;

	struct option cmdopts[] = {
		{"key", required_argument, NULL, 1},
		{"from-file", required_argument, NULL, 2},
		{"to-file", required_argument, NULL, 3},
		{"copyFromDDBoost", no_argument, NULL, 4},
		{"copyToDDBoost", no_argument, NULL, 5},
		{"del-dir", required_argument, NULL, 6},
		{"getFreePercent", no_argument, NULL, 7},
		{"del-file", required_argument, NULL, 8},
		{"readFile", no_argument, NULL, 9},
		{"listDirectory", no_argument, NULL, 10},
		{"table", required_argument, NULL, 11},
		{"getLatestTimestamp", no_argument, NULL, 12},
		{"dir", required_argument, NULL, 13},
		{"database", required_argument, NULL, 14},
		{"sync", no_argument, NULL, 15},
		{"write-file-from-stdin", no_argument, NULL, 16},
		{"dd_boost_buf_size", required_argument, NULL, 18},
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
				/* Timestamp */
				dd_options->timestamp = Safe_strdup(optarg);
				pInputOpts->pszPassThroughParms = addPassThroughLongParm("gp-ddb-key", optarg, pInputOpts->pszPassThroughParms);
				dd_boost_enabled = 1;
				break;
			case 2:
				/* Directory name on gpdb */
				dd_options->from_file = Safe_strdup(optarg);
				break;
			case 3:
				/* Directory name on gpdb */
				dd_options->to_file = Safe_strdup(optarg);
				break;
			case 4:
				/* Cleanup the files on DDBoost */
				dd_options->copyFromDDBoost = true;
				break;
			case 5:
				/* Copy File from DDBoost */
				dd_options->copyToDDBoost = true;
				break;
			case 6:
				/* Cleanup specified directory */
				dd_options->deleteDir = Safe_strdup(optarg);
				break;
			case 7:
				/* Get free percent */
				dd_options->getFreePercent = true;
				break;
			case 8:
				/* Cleanup specified file */
				dd_options->deleteFile = Safe_strdup(optarg);
				break;
			case 9:
				/* read file and write to stdout */
				dd_options->readFile = true;
				break;
			case 10:
				/* List directory contents */
				dd_options->listDir = true;
				break;
			case 11:
				/* Schema.Table to restore */
				if (!optarg)
				{
					mpp_err_msg_cache(logError, progname, "Try \"%s --help\" for more information.\n", progname);
					return false;
				}
				insertTableName(optarg, &dd_options);	
				break;
			case 12:
				dd_options->getLatestTimestamp = true;
				break;
			case 13:
				dd_options->directory = Safe_strdup(optarg);
				if (!optarg)
				{
					mpp_err_msg_cache(logError, progname, "DDBoost directory name missing\n");
					return false;
				}	
				break;
			case 14:
				dd_options->database = Safe_strdup(optarg);
				break;
				/* sync cdatabase, post_data and global dump files */
			case 15:
				dd_options->syncFiles = true;
				break;	
			case 16:
				/* Pipe input from stdout to DDboost file */
				dd_options->writeToDDFileFromInput = true;
				break;
			case 18:
				/* set I/O size for DD Boost */
				dd_boost_buf_size = atoi(optarg);
				if (dd_boost_buf_size < 64*1024 || dd_boost_buf_size > 1024*1024)
				{
					mpp_err_msg(logError, progname, "dd_boost_buf_size shoule be between 64KB and 1MB\n");
					dd_boost_buf_size = 512*1024; /* reset to default value */
				}
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
    ddp_uint64_t rw_size = dd_boost_buf_size;
    ddp_uint64_t total_bytes = 0;
    size_t written = 0;
    ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
 
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
	if (storage_unit_name == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		return -1;
	}
    sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		err = -1;
		goto cleanup;
	}	
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
    path1.path_name = full_path;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"ddboost File %s open failed. Err %d\n", path1.path_name, err);
        err = -1;
		goto cleanup;
    }

    buf = (char*)malloc(rw_size);
    if (buf == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
        goto cleanup;
    }

    do
    {
        memset(buf, 0, rw_size);
        ret_count = 0;
		
		err = ddp_stat(ddp_conn, &path1, &stat_buf);
		if (err)
        {
            mpp_err_msg(logError, progname, "ddboost stat failed on %s with error %d\n", path1.path_name, err);
            break;
        }
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

        err = ddp_read(handle, buf, rw_size,
                rw_size * i, &ret_count);
        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost read failed on %s with err %d\n", path1.path_name, err);
            break;
        }
		
        written = fwrite(buf, 1, ret_count, fp);

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0));

    fclose(fp);
	ddp_close_file(handle);

cleanup:
	if (buf)	
		free(buf);
	if (full_path)
		free(full_path);
	if (storage_unit_name)
		free(storage_unit_name);

    return err;
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

	/* Close both files in the called function */
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

	/* Close both the files in the called function */
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
		mpp_err_msg(logError, progname, "Directory on DDboost is not specified\n");
		return -1;
	}	
        
    storage_unit_name = (char*)malloc(MAX_PATH_NAME);
    sprintf(storage_unit_name, "%s", "GPDB");
	if (storage_unit_name == NULL)
	{
		err= -1;
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
		goto cleanup;
	}
	
	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", ddboostDir);
	if (full_path == NULL)
	{
		err = -1;
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
		goto cleanup;
	}

	path1.su_name = storage_unit_name;
	path1.path_name = ddboostDir;

	err = ddBoostRmdir(ddboostDir, ddp_conn, ddboostDir);
	if (err)
	{
		mpp_err_msg(logError, progname, "dboost Deleting Directory %s failed with err %d\n", path1.path_name, err);
		err = -1;
		goto cleanup;
	}

cleanup:
	if (storage_unit_name)
		free(storage_unit_name);
	if (full_path)
		free(full_path);

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
		mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Err %d\n", dir_path, err);
		return -1;
	}

	cur_path = (char*)malloc(MAX_PATH_NAME);
	if (!cur_path)
	{
		mpp_err_msg(logError, progname, "Memory allocation failure\n");
		return -1;
	}
	memset(cur_path, 0, MAX_PATH_NAME);	

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (!full_path)
	{
		mpp_err_msg(logError, progname, "Memory allocation failure\n");
		err = -1;
		goto cleanup;
	}
	memset(full_path, 0 , MAX_PATH_NAME);

	strcat(full_path, parent_dir);
	if (strcmp(&(full_path[strlen(full_path) - 1]), "/"))
		strcat(full_path, "/");

	/* Try to remove the directory. If the err is DD_ERR_NOTEMPTY then recursively delete the contents */
	err = ddp_rmdir(ddp_conn, &temp_path);
	if (err && err != DD_ERR_NOTEMPTY)
	{
		mpp_err_msg(logError, progname, "DDboost Directory %s deletion failed with err %d\n", temp_path.path_name, err);
		err = -1;
		goto cleanup;
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
				goto cleanup;
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
                    mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed with err %d\n", cur_path, err);
                    err = -1;
					goto cleanup;
				}
					
			}	
			else if (err)
			{
				/* Deleting file failed with error */
                mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed with err %d\n", cur_path, err);
                err = -1;
                goto cleanup;
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
        mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed with err %d\n", temp_path.path_name, err);
        err = -1;
        goto cleanup;
    }
	else
	{
		/* Successfully deleted directory */	
		err = 0;
	}

cleanup:
	if (cur_path)
		free(cur_path);
	if (full_path)
		free(full_path);

	return err;
}

static int writeToDDFile(FILE *fp, char *ddBoostFileName)	
{
	ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
    int err = 0;
    ddp_uint64_t ret_count = 0;
    char *buf = NULL;
    int i = 0;
    ddp_uint64_t rw_size = dd_boost_buf_size;
    ddp_uint64_t total_bytes = 0;
	ddp_uint64_t read_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
 
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
	if (storage_unit_name == NULL)
	{	
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
		goto cleanup; 
	}
    sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
	{	
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
		goto cleanup; 
	}
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
    path1.path_name = full_path;

	err = createDDboostDir(ddp_conn, storage_unit_name, ddBoostFileName);
	if (err)
	{
		mpp_err_msg(logError, progname, "Creating path %s on ddboost failed. Err %d\n", ddBoostFileName, err);
		err = -1;
		goto cleanup;			
	}

    err = ddp_open_file(ddp_conn, &path1, DDP_O_CREAT | DDP_O_RDWR, 0600, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"ddboost File %s open failed. Err %d\n", path1.path_name, err);
        err = -1;
		goto cleanup;
    }

    buf = (char*)malloc(rw_size);
    if (buf == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
		goto cleanup; 
    }

    do
    {
        memset(buf, 0, rw_size);
        ret_count = 0;

        read_bytes = fread(buf, 1, rw_size, fp);

        err = ddp_write(handle, buf, read_bytes,
                rw_size * i, &ret_count);

        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost write failed on %s with err %d\n", path1.path_name, err);
            err = -1;
            break;
        }

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0) && (rw_size == read_bytes));

	err = ddp_stat(ddp_conn, &path1, &stat_buf);
    if (err)
    {
        mpp_err_msg(logError, progname, "ddboost stat failed on %s failed with err %d\n", path1.path_name, err);
        err = -1;
        goto cleanup;
    } 
		
	if (stat_buf.st_size < rw_size )
		rw_size = stat_buf.st_size;	

    fclose(fp);
	ddp_close_file(handle);

cleanup:
	if (full_path == NULL)
		free(full_path);
	if (storage_unit_name)
		free(storage_unit_name);

        return err;
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
    {
        mpp_err_msg(logError, progname, "ddboost no file specified for delete\n");
		return -1;
    }

	temp = (char*)malloc(MAX_PATH_NAME);
	if (!temp)
	{
        mpp_err_msg(logError, progname, "ddboost memory allocation failed\n");
		return -1;
	}

	memset(temp, 0, MAX_PATH_NAME);

	strcat(temp, dd_options->deleteFile);

	temp_path.path_name = temp;
	temp_path.su_name = "GPDB";
	
	err = ddp_unlink(ddp_conn, &temp_path);		
	if (err)
	{
        mpp_err_msg(logError, progname, "File %s cannot be deleted on DDboost. Error %d\n", temp_path.path_name, err);
	}

	free(temp);
	return err;
}

int
readFromDDFileToOutput(char *ddBoostFileName)
{
    ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
    int err = 0;
    ddp_uint64_t ret_count = 0;
    char *buf = NULL;
    int i = 0;
    ddp_uint64_t rw_size = dd_boost_buf_size;
    ddp_uint64_t total_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
    if (storage_unit_name == NULL)
	{
		mpp_err_msg(logError, progname,("Memory allocation failed\n"));
        err = -1;
        goto cleanup;
	}	
	sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
	{
		mpp_err_msg(logError, progname,("Memory allocation failed\n"));
		err = -1;
		goto cleanup;	
	}
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
    path1.path_name = full_path;
	
    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"File %s open on ddboost failed. Err %d\n", path1.path_name, err);
       	err =-1;
		goto cleanup;
    }

    buf = (char*)malloc(rw_size);
    if (buf == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
       	err = -1;
		goto cleanup;
    }

   	do
   	{
        memset(buf, 0, rw_size);
        ret_count = 0;
		
		err = ddp_stat(ddp_conn, &path1, &stat_buf);
		if (err)
        {
               	mpp_err_msg(logError, progname, "ddboost stat failed on %s with err %d\n", path1.path_name, err);
               	err = -1;
		        break;
        }
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

        err = ddp_read(handle, buf, rw_size,
                rw_size * i, &ret_count);
        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost read failed on %s with err %d\n", path1.path_name, err);
            err = -1;
            break;
        }
	
		write(1, buf, ret_count);

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0));

cleanup:
	free(storage_unit_name);
	free(full_path);
    return err;
}

int 
createDDboostDir(ddp_conn_desc_t ddp_conn, char *storage_unit_name, const char *filePath)
{
	char *pch = NULL;
    ddp_path_t path = {0};
    int err = 0;
    char *full_path = NULL;
	char *path_name = strdup(filePath);

	if (!filePath)
	{
		mpp_err_msg(logError, progname, "Specify path for directory\n");
		return -1;
	}

    full_path = (char*)malloc(MAX_PATH_NAME);
    memset(full_path, 0, MAX_PATH_NAME);
	if (filePath[0] == '/')
		strcat(full_path, "/");

    pch = strtok(path_name, " /");
    while(pch != NULL)
    {
        path.su_name = storage_unit_name;
        strcat(full_path, pch);
        path.path_name = full_path;

		if (strncmp(full_path, filePath, strlen(filePath)))
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

	free(full_path);
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
        mpp_err_msg(logError, progname, "Directory on DDboost is not specified\n");
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
		free(storage_unit_name);
		free(full_path);
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Err %d\n", ddboostDir, err);
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
                mpp_err_msg(logError, progname, "Reading directory %s on ddboost failed. Err %d\n", ddboostDir, err);
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

	free(storage_unit_name);
	free(full_path);
	return err;
}

/* For selective restore we need to parse the specified dump file
 * for psql statements corresponding to the required table 
 */
static int 
createFakeRestoreFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
    ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
    int err = 0;
    ddp_uint64_t ret_count = 0;
    char *buf = NULL;
    int i = 0;
    ddp_uint64_t rw_size = dd_boost_buf_size;
    ddp_uint64_t total_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
	int fd[2];
	char line[101];
	int done = false;
	char *endString = NULL;
	int output, in_schema, found;
	char *curSchema = NULL;
	void *ddfp = NULL, *ddfpTemp = NULL;
	int isCompress = 0;

	if (!dd_options->to_file)
	{
        mpp_err_msg(logError, progname, "Output file not specified for selective restore\n");    
		return -1;
	}

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
    sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", dd_options->from_file);

	path1.su_name = storage_unit_name;
    path1.path_name = full_path;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        free(storage_unit_name);
        free(full_path);
        mpp_err_msg(logError, progname,"File %s open on ddboost failed. Err %d\n", path1.path_name, err);
        return -1;
    }

    buf = (char*)malloc(rw_size);
    if (buf == NULL)
    {
		free(storage_unit_name);
		free(full_path);
       	mpp_err_msg(logError, progname, "Memory allocation failed\n");
       	return -1;
    }

	isCompress = isFileCompressed(full_path);

    /* Always try to create the Directory db_dumps and db_dumps/<date> before we start the
     * creation of fake dump files. If the directories already exist, then ignore the error.
     * This creation is important beacause, when we do a selective restore of an old backup 
     * onto a fresh cluster, the segment directories wont have the db_dumps and db_dumps/<date>
     * directories */
    err = createDbdumpsDir(dd_options->to_file);
    if (err)
    {
        mpp_err_msg(logError, progname, "Creating directory %s on ddboost failed. Err %d\n", dd_options->to_file, err);
        return -1;
    }

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
            {
                mpp_err_msg(logError, progname, "ddboost stat failed on  %s Err %d\n", path1.path_name, err);
				break;
            }
		
			if (stat_buf.st_size < rw_size )
				rw_size = stat_buf.st_size;	

           	err = ddp_read(handle, buf, rw_size,
           	rw_size * i, &ret_count);
           	if (err)
            {
                mpp_err_msg(logError, progname, "ddboost read failed on %s Err %d\n", path1.path_name, err);
                break;
            }
	
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

        ddfp = DDDOPEN(fd[0], "r", isCompress);
        if (ddfp == NULL)
        {
            mpp_err_msg(logError, progname, "ddboost read failed on %s \n", path1.path_name);
            return -1;
        }

		ddfpTemp = DDOPEN(dd_options->to_file, "w", isCompress);
		if (ddfpTemp == NULL)
		{
            mpp_err_msg(logError, progname, "Opening file %s failed\n", dd_options->to_file);
			DDCLOSE(ddfp, isCompress);
			return -1;
		} 

		endString = (char*)malloc(MAX_PATH_NAME);
		memset(endString, 0, MAX_PATH_NAME);
		sprintf(endString, "\\.");
	
		output = false;
		in_schema = false;
		found = false;
	
	
		do
		{
			memset(line, 0, 101);

            if (DDGETS(line, 100, ddfp, isCompress) != NULL)
			{
				if (lineStartsWithSchema(line, dd_options->tableList, &curSchema))
				{
					DDWRITE(line, 1, strlen(line), ddfpTemp, isCompress);
					in_schema = true;
				}
				else if (in_schema && lineStartsWithTable(line, dd_options->tableList, curSchema))
				{
					found = true;
					output = true;
					DDWRITE(line, 1, strlen(line), ddfpTemp, isCompress);
				}
				else if (lineEndsWith(line, endString))
				{
					if (found && in_schema)
					{
						DDWRITE(line, 1, strlen(line), ddfpTemp, isCompress);
						output = false;
						found = false;
					}
				}	
				else if (found && output)
				{
					DDWRITE(line, 1, strlen(line), ddfpTemp, isCompress);
				}
			}
			else
			{
				DDCLOSE(ddfp, isCompress);		
				DDCLOSE(ddfpTemp, isCompress);		
				done = true;
			}
		} while (!done);
		close(fd[0]);
		ddp_close_file(handle);
	}
	free(storage_unit_name);
    free(full_path);
	free(buf);	
	return 0;
}

/* check if the line starts with the specified pattern */
int 
lineStartsWithSchema(char *line, struct schemaTableList *tableList, char **curSchema)
{
	int i = 0;
	char schemaLine[MAX_PATH_NAME];
	struct schemaTableList *cur = tableList;
	
	/* Skip spaces at the beginning */	
	while (line[i] == ' ')
		i++;

	/* For every schema check if the line begins with SET SEARCH PATH <schema name> */
	while (cur != NULL)
	{
		memset(schemaLine, 0, MAX_PATH_NAME);
		sprintf(schemaLine, "SET search_path = %s", cur->schemaName); 
		if (strstr(line, schemaLine))
		{
			*curSchema = strdup(cur->schemaName);
			return 1;
		}
		cur = cur->next;
	}
	return 0;
} 

/* check if the line starts with the specified pattern */
int 
lineStartsWithTable(char *line, struct schemaTableList *tableList, char *curSchema)
{
	int i = 0;
	char schemaLine[MAX_PATH_NAME];
	struct schemaTableList *cur = tableList;
	
	/* Skip spaces at the beginning */	
	while (line[i] == ' ')
		i++;

	/* For every Table check if the line begins with COPY <table name> */
	while (cur != NULL)
	{
		memset(schemaLine, 0, MAX_PATH_NAME);
		sprintf(schemaLine, "COPY %s", cur->tableName); 
		if ((!strcmp(curSchema, cur->schemaName) && (strstr(line, schemaLine))))
			return 1;
		cur = cur->next;
	}
	return 0;
} 
/* Check if the line ends with the specified pattern */	
int 
lineEndsWith(char *line, char *pattern)
{
	if (strstr(line, pattern))
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
        mpp_err_msg(logError, progname, "Directory on DDboost is not specified\n");
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
        mpp_err_msg(logError, progname, "Directory %s open failed on ddboost. Err %d\n", ddboostDir, err);
        return -1;
    }

	recentTimestamp = (char*)malloc(MAX_PATH_NAME);
	if (!recentTimestamp)
	{
		free(storage_unit_name);
        free(full_path);
        mpp_err_msg(logError, progname, "Memory allocation failed in gpddboost\n");
		return -1;
	}	
	memset(recentTimestamp, 0, MAX_PATH_NAME);

	filePath = (char*)malloc(MAX_PATH_NAME);
	if (!filePath)
	{
		free(recentTimestamp);
		free(storage_unit_name);
       	free(full_path);
        mpp_err_msg(logError, progname, "Memory allocation failed in gpddboost\n");
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
                mpp_err_msg(logError, progname, "ddboost readdir on %s failed. Err %d\n", ddboostDir, err);
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
			if (strncmp(ret_dirent.d_name, "gp_cdatabase", strlen("gp_cdatabase")))
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
	
	if (found)
		printf("\n%s\n", &recentTimestamp[strlen(recentTimestamp)-14]);
	else
		printf("\nNo dump file with the specified database exists\n");

	free(recentTimestamp);
	free(storage_unit_name);
    free(full_path);
		
	return err;
}


int
dumpFileHasDatabaseName(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *fullPath, char *databaseName)
{
	/* Read the first 5 lines of the cdatabase file on DDboost */
    ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
    int err = 0, ret = 0;
    ddp_uint64_t ret_count = 0;
    char *buf = NULL;
    ddp_uint64_t rw_size = dd_boost_buf_size;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	ddp_stat_t stat_buf;
	char *searchString = NULL;

	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
    sprintf(storage_unit_name, "%s", "GPDB");
	if (storage_unit_name == NULL)
	{
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
		ret = 0;
		goto cleanup;
	}
	
	full_path = (char*)malloc(MAX_PATH_NAME);
	sprintf(full_path, "%s", fullPath);
	if (full_path == NULL)
	{
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
		ret = 0;
		goto cleanup;
	}

	path1.su_name = storage_unit_name;
    path1.path_name = full_path;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname, "ddboost file  %s open failed. Err %d\n", full_path, err);
    	ret = 0;
		goto cleanup;
    }

    buf = (char*)malloc(MAX_PATH_NAME + 1);
    if (buf == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
       	ret = 0;
		goto cleanup;
    }

	/* The CREATE DATABASE statement is usually in the first 5 lines */
    memset(buf, 0, MAX_PATH_NAME + 1);
    ret_count = 0;
		
	err = ddp_stat(ddp_conn, &path1, &stat_buf);
	if (err)
	{
		ret = 0;
		goto cleanup;
	}	
		
	if (stat_buf.st_size < MAX_PATH_NAME )
		rw_size = stat_buf.st_size;	

    err = ddp_read(handle, buf, MAX_PATH_NAME,
              	0, &ret_count);

    if (err)
	{
        mpp_err_msg(logError, progname, "Reading file %s on ddboost failed. Err %d\n", path1.path_name, err);
        ret = 0;
		goto cleanup;
	}
	
	searchString = (char*)malloc(MAX_PATH_NAME);
	memset(searchString, 0, MAX_PATH_NAME);
	sprintf(searchString, "CREATE DATABASE %s", databaseName);

	if (strstr(buf, searchString))
		ret = 1;

cleanup:
	if (buf)
		free(buf);
	if (full_path)
		free(full_path);
	if(storage_unit_name)
		free(storage_unit_name);
	
	return ret;
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
        mpp_err_msg(logError, progname, "Directory on DDboost is not specified\n");
        return -1;
    }

    storage_unit_name = (char*)malloc(MAX_PATH_NAME);
	if (storage_unit_name == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
        goto cleanup;
    }	
    sprintf(storage_unit_name, "%s", "GPDB");

    full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
    {
        mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
        goto cleanup;
    }
    sprintf(full_path, "%s", ddboostDir);

    path1.su_name = storage_unit_name;
    path1.path_name = ddboostDir;

	/* Start traversing from db_dumps/ directory */
	err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Err %d\n", ddboostDir, err);
        err = -1;
		goto cleanup;
    }

	/* Create the db_dumps directory if it doesn't exist already */
	dest_path = (char*)malloc(MAX_PATH_NAME);
	memset(dest_path, 0, MAX_PATH_NAME);
	sprintf(dest_path, "%s/%s", dirPath, ddboostDir); 
	
	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if (err && (err != EEXIST) && (err != -1))
	{
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed .Err %d\n", dest_path, err);
		err = -1;
		goto cleanup;	
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
                mpp_err_msg(logError, progname, "ddboost readdir failed on %s Err %d\n", ddboostDir, err);
                goto cleanup;
            }
        }
        else
        {
            if (strcmp("..", ret_dirent.d_name) == 0)
                continue;
                        
			if (strcmp(".", ret_dirent.d_name) == 0)
                continue;

			/* Only check directories of the form YYYYMMDD */
			if (!isdigit(ret_dirent.d_name[0]))
				continue;

	 		/* For every date, copy the required files over to GPDB */
			memset(full_path, 0, MAX_PATH_NAME);
			sprintf(full_path, "%s/%s", ddboostDir, ret_dirent.d_name);

			copyFilesFromDir(full_path, dirPath, ddp_conn);
		}
	}

cleanup:
	if (storage_unit_name)
		free(storage_unit_name);
	if (full_path)
		free(full_path);
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
        mpp_err_msg(logError, progname, "Directory on DDboost is not specified\n");
        return -1;
    }

    storage_unit_name = (char*)malloc(MAX_PATH_NAME);
	if (storage_unit_name == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		err = -1;
		goto cleanup;
	}
    sprintf(storage_unit_name, "%s", "GPDB");

    full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
        err = -1;
        goto cleanup;
	}
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
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Err %d\n", ddboostDir, err);
		err = -1;
		goto cleanup;
    }

	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if (err && (err != EEXIST) && (err != -1))
	{
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed with error %d\n", dest_path, err);
		err = -1;
		goto cleanup;
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
                mpp_err_msg(logError, progname, "ddboost read dir failed on %s with error %d\n", ddboostDir, err);
                goto cleanup;
            }
        }
        else
        {
            if (strcmp("..", ret_dirent.d_name) == 0)
                continue;
                        
			if (strcmp(".", ret_dirent.d_name) == 0)
                continue;

			if (!isFileToBeCopied(ret_dirent.d_name))
			{
				continue;
			}

	 		/* For every date, copy the required files over to GPDB */
			memset(full_path, 0, MAX_PATH_NAME);
			sprintf(full_path, "%s/%s", ddboostDir, ret_dirent.d_name);

			memset(dest_path, 0, MAX_PATH_NAME);
			sprintf(dest_path, "%s/%s", toDir, full_path);

			fp = fopen(dest_path, "w");
			if (!fp)
			{
                mpp_err_msg(logError, progname, "File %s open failed on GPDB\n", dest_path);
				err = -1;
				goto cleanup;	
			}

			err = readFromDDFile(fp, full_path);
		}
	}

cleanup:
	if (full_path)
		free(full_path);

	if (storage_unit_name)
		free(storage_unit_name);

	return err;
}

int 
isFileToBeCopied(const char *filename)
{
	if (!strncmp(filename, "gp_global_1_1_", strlen("gp_global_1_1_")))
		return 1;

	if (!strncmp(filename, "gp_cdatabase_1_1_", strlen("gp_cdatabase_1_1_")))
		return 1;

	if (strstr(filename, "_post_data"))
		return 1;

	if (strstr(filename, ".tar"))
		return 1;

	return 0;
}

/* 
 * writeToDDFileFromInput
 * read data stream from standard input and write out to DD system
 */
static int writeToDDFileFromInput(char *ddBoostFileName)
{
	ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
	int err = 0;
	ddp_uint64_t ret_count = 0;
	char *buf = NULL;
	ddp_uint64_t rw_size = dd_boost_buf_size;
	ddp_uint64_t total_bytes = 0;
	ddp_uint64_t read_bytes = 0;
	ddp_path_t path1 = {0};
	char *storage_unit_name = NULL;
	char *full_path = NULL;       
	char *buf_iogroup = NULL;
	int   buf_data_length = 0;

	if (!ddBoostFileName)
	{
		mpp_err_msg(logError, progname, "Destination filename not specified\n");
		return -1;
	}

	if (ddBoostFileName[strlen(ddBoostFileName)-1] == '/')
	{
		mpp_err_msg(logError, progname, "Invalid filename specified. Filename cannot end with /\n"); 
		return -1;
	}
 
	storage_unit_name = (char*)malloc(MAX_PATH_NAME);
	if (storage_unit_name == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		return -1;
	}
	sprintf(storage_unit_name, "%s", "GPDB");

	full_path = (char*)malloc(MAX_PATH_NAME);
	if (full_path == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		err = -1;
		goto cleanup;
	}
	sprintf(full_path, "%s", ddBoostFileName);

	path1.su_name = storage_unit_name;
	path1.path_name = full_path;

	err = createDDboostDir(ddp_conn, storage_unit_name, ddBoostFileName);
	if (err)
	{
		mpp_err_msg(logError, progname, "ddboost Creating path %s failed. Err %d\n", ddBoostFileName, err);
		err = -1;
		goto cleanup;	
	}

	err = ddp_open_file(ddp_conn, &path1, DDP_O_CREAT | DDP_O_RDWR, 0600, &handle);
	if (err)
	{
		mpp_err_msg(logError, progname,"ddboost File %s open failed. Err %d\n", path1.path_name, err);
		err = -1;
		goto cleanup;
	}

	buf = (char*)malloc(rw_size);
	if (buf == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		err = -1;
		goto cleanup;
	}

	buf_iogroup = (char*) malloc(rw_size);
	if (buf_iogroup == NULL)
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		err = -2;
		goto cleanup;
	}
	memset(buf_iogroup, 0, rw_size);

	do
	{
		memset(buf, 0, rw_size);
		ret_count = 0;

		read_bytes = read(0, buf, rw_size);
		if (read_bytes > 0)
		{
			if (read_bytes == rw_size) /* no I/O grouping is needed */
			{
				/* flushing existing data in I/O grouping buffer first */
				if (buf_data_length > 0)
				{
					err = ddp_write(handle, buf_iogroup, buf_data_length, total_bytes, &ret_count);
					if (err)
                    {
                        mpp_err_msg(logError, progname,"ddboost File %s write failed. Err %d\n", path1.path_name, err); 
						break;
                    }
					total_bytes += ret_count;
					memset(buf_iogroup, 0, rw_size);
					buf_data_length = 0;
				}


				err = ddp_write(handle, buf, read_bytes,
						total_bytes, &ret_count);
				if (err)
                {
                    mpp_err_msg(logError, progname,"ddboost File %s write failed. Err %d\n", path1.path_name, err); 
					break;
                }
				total_bytes += ret_count;
			}
			else	/* grouping small I/O */ 
			{
				if ((read_bytes + buf_data_length) <= rw_size)
				{
					memcpy(buf_iogroup + buf_data_length, buf, read_bytes);
					buf_data_length += read_bytes;
				}
				else
				{
					err = ddp_write(handle, buf_iogroup, buf_data_length,
							total_bytes, &ret_count);
					if (err)
                    {
                        mpp_err_msg(logError, progname,"ddboost File %s write failed. Err %d\n", path1.path_name, err); 
						break;
                    }
					total_bytes += ret_count;
					
					memset(buf_iogroup, 0, rw_size);
					memcpy(buf_iogroup, buf, read_bytes);
					buf_data_length = read_bytes;	
				}
			}
		}
	} while((err == 0) && (read_bytes > 0));

	/* skip writing remaining data if we already met I/O error */
	if ((err == 0) && (buf_data_length > 0))
		err = ddp_write(handle, buf_iogroup, buf_data_length, total_bytes, &ret_count);
		
	if (err)
	{
		mpp_err_msg(logError, progname, "ddboost File %s write failed Err %d\n", path1.path_name, err);
		err = -1;
	}

cleanup:
	if (storage_unit_name)
		free(storage_unit_name);
	if (full_path)
		free(full_path);
	if (buf)
		free(buf);
	if (buf_iogroup)
		free(buf_iogroup);

	ddp_close_file(handle);
	return err;
}

int 
isFileCompressed(const char *filename)
{
	if (!filename)
		return 0;

	if (strstr(filename, ".gz"))
	{
		return 1;
	}
	
	return 0;
}

int
createDbdumpsDir(char *filePath)
{
    char *pch = NULL;
    char *filePathCopy = NULL;
    int found = 0;
    char curPath[MAX_PATH_NAME];
    int err= 0;

    memset(curPath, 0, MAX_PATH_NAME);
    if (filePath[0] == '/')
        curPath[0]='/';

    /* Retain a copy of the original pathname as strtok strips off the string */
    filePathCopy = strdup(filePath);

    pch = strtok(filePathCopy, " /");
    while(pch != NULL)
    {
        /* Create only the db_dumps and the next directory in the hierarchy */
        if (!found && (strcmp(pch, "db_dumps") == 0))
            found = 1;

        strcat(curPath, pch);

        /* Do not create the file here */
        if (strncmp(curPath, filePath, strlen(filePath)))
        {
            if (found)
            {
                err = mkdir(curPath, S_IRWXU);
                if (err && (err != EEXIST) && (err != -1))
                {
                    mpp_err_msg(logError, progname,"Directory %s creation on GPDB path failed with error %d\n", curPath, err);
                    return -1;
                }
        
                /* The directory structure is <some_path>/db_dumps/<date>/
                 * When we finish creating the dated directory, we need to stop */
                if (isdigit(pch[0]))
                    return 0;
            }

            strcat(curPath, "/");
            pch = strtok(NULL, " /");
        }
        else
            break;
    }
    return 0;
}

#else
int main(int argc, char **argv)
{
	printf("\ngpddboost not supported on this platform\n");	
	return 0;
}	
#endif
