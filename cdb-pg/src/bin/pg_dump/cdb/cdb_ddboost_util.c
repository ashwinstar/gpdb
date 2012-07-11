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

#define logInfo  "INFO"
#define logError "ERROR"
static const char *progname;

#ifdef USE_DDBOOST
#include "ddp_api.h"
#include "clb.h"
#include "ddboost/cdb_ddboost_util.h"

#define MAX_PATH_NAME 1024
#define DDB_POOL_SIZE (32 * 1024 * 2048)

#define DDP_FILECOPY_EXTENT_LEN 			(262144)
#define DDP_MAX_NUMBER_OF_STORAGE_UNITS 	(255) /* DataDomain constraint */

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

/* We allow to store more than one host. 	*/
/* This struct will hold all the hosts		*/
typedef struct hostNameListStruct
{
	char 						*hostName;
	struct hostNameListStruct 	*next;
} hostNameList;

struct ddboost_options
{
  char *timestamp;
  char *directory;
  char *from_file;
  char *to_file;
  char *deleteDir;
  char *deleteFile;
  hostNameList *hostsList;
  char *user;
  char *password;
  char *log_level;
  char *log_size;
  struct schemaTableList *tableList;
  char *database;
  char *syncFilesFromTimestamp;
  bool listDir;
  bool listDirFull;
  bool copyFromDDBoost;	
  bool copyToDDBoost;
  bool copy;
  bool readFile;	
  bool getFreePercent;
  bool getLatestTimestamp;
  bool syncFiles;
  bool writeToDDFileFromInput;
  bool rename;
  bool setCredential;
  bool verify;
  bool listSu;
  char *createSu;
};

static struct ddboost_options *dd_options = NULL;

/* Holds the info of the current DD connection */
extern ddboost_connection_info dd_conn_info;

/* Forward declarations */
static bool fillInputOptions(int argc, char **argv, InputOptions * pInputOpts);
static bool parmValNeedsQuotes(const char *Value);
static void usage(void);
static int readFromDDFile(FILE *fp, char *ddBoostFileName);	
static int readFromDDFileToOutput(char *ddBoostFileName);
static int writeToDDFileFromInput(char *ddBoostFileName);
static int writeToDDFile(FILE *fp, char *ddBoostFileName);	

typedef struct option optType;

static RestoreOptions *opts = NULL;

static char * addPassThroughLongParm(const char *Parm, const char *pszValue, char *pszPassThroughParmString);
static char * shellEscape(const char *shellArg, PQExpBuffer escapeBuf);
static int dd_boost_enabled = 1;

extern ddp_client_info_t dd_client_info;

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
static int listDirectoryFull(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int syncFilesFromDDBoostTimestamp(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath);
static int renameFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int setCredential(struct ddboost_options *dd_options);
static int copyWithinDDboost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int listAllStorageUnits(ddp_conn_desc_t ddp_conn);
static int getStorageUnitsListFromOneHost(char *hostName, ddp_su_list_t *su_list);
static int createStorageUnitOnAllHosts(ddp_conn_desc_t ddp_conn, const char *storage_unit);
static int createCurrentStorageUnitOnAllHosts(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn);
static int addNewHostName(struct ddboost_options *dd_options, char *hostName);
static void freeDdOptions(struct ddboost_options *dd_options);
static void *ddMalloc(size_t size);

/* Helper routines for handling multiple -T options using a linked list */
static struct schemaTableList *getNode(char *schemaTable)
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

static void insertTableName(char *schemaTable, struct ddboost_options **dd_options)
{
    struct schemaTableList *temp = NULL;

    temp = getNode(schemaTable);
    temp->next = (*dd_options)->tableList;
    (*dd_options)->tableList = temp;
}

static void deleteTableName(char *table, char *schema, struct ddboost_options **dd_options)
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

static void cleanupTableList(struct ddboost_options **dd_options)
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
	int	failCount = 0;
	int	ret = 0;

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

	if (dd_options->setCredential)
	{
		failCount = setCredential(dd_options);
		goto cleanup;
	}

    /* Connect to the DD system. This is also initializes the DD context if needed */
    ret = connectToDDSystem(&ddp_inst, &ddp_conn, &dd_client_info, &DDP_SU_NAME);

    if (ret != DD_OK)
    {
		mpp_err_msg(logError, progname, "Initialization of ddboost failed. Check login credentials\n");
		failCount = 1;
    	goto cleanup;
    }

    /* The verification was actually performed, since initDDSystem() returned OK 	*/
    /* So we just need to create the SU on the DDR									*/
    if (dd_options->verify)
    {
    	failCount = createCurrentStorageUnitOnAllHosts(dd_options, ddp_conn);
    	goto cleanup;
    }

    /* List all the available storage units on all the available Data Domain appliances */
	if (dd_options->listSu)
	{
		failCount = listAllStorageUnits(ddp_conn);
		goto cleanup;
	}

	if (dd_options->createSu)
	{
		failCount = createStorageUnitOnAllHosts(ddp_conn, dd_options->createSu);
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

    if (dd_options->copy)
    {
        failCount = copyWithinDDboost(dd_options, ddp_conn);
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

    if (dd_options->listDirFull)
    {
		if (!dd_options->directory)
		{
			mpp_err_msg(logError, progname, "Directory not specified for listDir\n");
			goto cleanup;
		}

        failCount = listDirectoryFull(dd_options, ddp_conn);
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

    if (dd_options->syncFilesFromTimestamp)
    {
        failCount = syncFilesFromDDBoostTimestamp(dd_options, ddp_conn, dd_options->directory);
        goto cleanup;
    }

	if (dd_options->writeToDDFileFromInput)
	{
		failCount = writeToDDFileFromInput(dd_options->to_file);
		goto cleanup;
	}

    if (dd_options->rename)
    {
        failCount = renameFile(dd_options, ddp_conn);
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

    ddp_shutdown();

	if (dd_options != NULL)
	{
		freeDdOptions(dd_options);
		free(dd_options);
	}
	
	return (failCount == 0 ? 0 : 1);
}

static void
usage(void)
{
	printf(("\n%s Utility routine to access DDBoost from Greenplum Database\n\n"), progname);
    printf("WARNING !! gpddboost should not be used as a standalone utility\n\n");
    printf(("  %s [OPTIONS]\n"), progname);

	printf(("\nGeneral options:\n"));
    printf("  --copyFromDDBoost   --from-file=<ddboost_full_path_name>   --to-file=<local_disk_full_path_name> \n");  
    printf("  Copy a file from DDBoost to GPDB\n");	
    printf("  The destination directory on local disk should exist.\n\n\n");
    printf("  --copyToDDBoost    --from-file=<local_disk_full_path_name> --to-file=<ddboost_full_path_name> \n");  
    printf("  Copy a file from GPDB to DDBoost.\n\n\n");
    printf("  --copy   --from-file=<ddboost_source_full_path_name> --to-file=<ddboost_dest_full_path_name> \n");  
    printf("  Copy a file from one location to another within DDBoost.\n\n\n");  
    printf("  --del-dir=<ddboost_full_path_name>			\n");
    printf("  To delete a directory on DDboost specified by ddboost_full_path_name.\n\n\n");	
    printf("  --del-file=<ddboost_full_path_name>           \n");
    printf("  To delete a specific file on DDboost.\n\n\n"); 
    printf("  --listDirectory  --dir=<ddboost_directory_full_path_name> \n");
    printf("  To list the contents of a directory on DDboost. \n");
    printf("  If ddboost_directory_full_path_name is empty or / then the contents of the root directory \n");
    printf("  in the default storage unit is displayed.\n\n\n");
    printf("  --ls   --dir=<ddboost_directory_full_path_name>\n");	
    printf("  To list the size and permissions of the contents of a directory on DDboost.\n"); 
    printf("  If ddboost_directory_full_path_name is empty or / then the contents of the root directory \n");
    printf("  in the default storage unit is displayed.\n\n\n");
    printf("  --readFile	--from-file=<ddboost_full_path_name>\n");
    printf("  To read a file on DDBoost. By default the output is redirected to standard output.\n\n\n");	 
    printf("  --write-file-from-stdin 	--to-file=<ddboost_full_path_name>\n");
    printf("  To write a file to DDBoost from stdin.\n\n\n");
    printf("  --table=<schemaName.tableName>  --from-file=<ddboost_full_path_name>  --to-file=<local_disk_full_path_name>\n");
    printf("  To create a fake backup file containing only the data for the specified table. \n");
    printf("  The destination directory should exist on the local disk.\n\n\n");        
    printf("  --setCredential  --hostname <DD_host>  --user=<DD_user>"); 
    printf("  [--password=<DD_password>  --logLevel=<NONE, ERROR, WARN, INFO, DEBUG> --logSize=<1-1000 (MB)>]\n");
    printf("  Set the ddboost login credentials. Hostname and user are required.\n");
    printf("  If the password option is not specified, it will be interactivly requested.\n");
    printf("  If the log options are not specified, the default values will be used (level will be DEBUG and size 50 MB).\n\n\n");
    printf("  --sync	--dir=<local_disk_full_path_name>\n");
    printf("  To copy over the config, global dump, post_data and cdatabase files from DDBoost to GPDB\n"); 
    printf("  Generally we specify the master data directory as the destination.\n\n\n");		
    printf("  --syncTimestamp=<timestamp>     --dir=<local_disk_full_path_name>\n");
    printf("  To sync all the files having a particular timestamp from DDboost to local disk\n");
    printf("  The destination is specified by local_disk_full_path_name and it needs to exist.\n\n\n");
    printf("  --dd_boost_buf_size=<size_in_bytes>\n");
    printf("  DDboost I/O buffer size. Max IO buffer size is 1MB\n\n\n");
    printf("  --rename  --from-file=<ddboost_full_path_name_source>  --to-file=<ddboost_full_path_name_dest>\n");
    printf("  Rename file specified by --from-file to a destination specified by --to-file on DDBoost\n");
    printf("  Warning !! This operation will overwrite/delete an existing file if specified as destination.\n\n\n");
    printf("  --verify\n");
    printf("  Verify the DD_user and the DD_password on the DD_host\n");
    printf("  This will also create the Storage Unit.\n\n\n");
    printf("  --list-su\n\n\n");
	printf("  List all the available storage units on all the available Data Domain appliances\n");
	printf("  --create-su <Storage_Unit_Name>\n\n\n");
    printf("  Create new storage unit\n");
	printf("  --help\n");
    printf("  Show this help, then exit.\n\n\n");
    printf("  --version\n");
    printf("  Output version information, then exit.\n\n\n");
}

static bool
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
		{"ls", no_argument, NULL, 19},
		{"syncTimestamp", required_argument, NULL, 20},
		{"rename", no_argument, NULL, 21},
		{"copy", no_argument, NULL, 22},

		{"setCredential", no_argument, NULL, 23},
		{"hostname", required_argument, NULL, 24},
		{"user", required_argument, NULL, 25},
		{"password", required_argument, NULL, 26},
		{"logLevel", required_argument, NULL, 27},
		{"logSize", required_argument, NULL, 28},
		{"verify", no_argument, NULL, 29},
		{"list-su", no_argument, NULL, 30},
		{"create-su", required_argument, NULL, 31},

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
			int 				err;
			ddp_version_t 		ddp_ver;

			printf("gpddboost (%s)\n", dd_client_info.cl_info);

			if (initDDSystem(&ddp_inst, &dd_client_info) == 0)
			{
				err = ddp_get_version(&ddp_ver);
				if (err == DD_ERR_NONE)
				{
					printf("DD Client Version is: [%d.%d.%d.%d.%s]\n",
						  ddp_ver.v_major, ddp_ver.v_minor, ddp_ver.v_patch,
						  ddp_ver.v_engineering, ddp_ver.v_build);
				}
			}
			exit(0);
		}
	}

	/*
	 * Record the command line params as a string for documentation in the
	 * output report
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
            case 19:
                /* Long listing of directories */
				dd_options->listDirFull = true;
                break;
            case 20:
                dd_options->syncFilesFromTimestamp = Safe_strdup(optarg);;
                break;
            case 21:
                dd_options->rename = true;
                break;
            case 22:
                dd_options->copy = true;
                break;

		    case 23:
		      	dd_options->setCredential = true;
		      	break;
		    case 24:
		    	if (addNewHostName(dd_options, optarg) != 0 )
		    		return false;
		        break;
		    case 25:
		        dd_options->user = Safe_strdup(optarg);
		        break;
		    case 26:
		        dd_options->password = Safe_strdup(optarg);
		        break;
		    case 27:
		        dd_options->log_level = Safe_strdup(optarg);
		        break;
		    case 28:
		        dd_options->log_size = Safe_strdup(optarg);
	      	    break;
            case 29:
		        dd_options->verify = true;
	      	    break;
            case 30:
				dd_options->listSu = true;
				break;
            case 31:
            	/* -1 is for the '\n' */
            	if (strlen(optarg) > DDP_MAX_SU_NAME-1)
            	{
            		mpp_err_msg_cache(logError, progname, "Storage unit name should be shorter than %d characters\n", DDP_MAX_SU_NAME);
					return false;
            	}
				dd_options->createSu = Safe_strdup(optarg);
				break;

			default:
				mpp_err_msg_cache(logError, progname, "Try \"%s --help\" for more information.\n", progname);
				return false;
		}
	}

	/* Check switches combinations and conflicts */
	if (dd_options->setCredential && !(dd_options->hostsList && dd_options->user))
	{
	    mpp_err_msg_cache(logError, progname, "When specifying the option --setCredential, the options --hostname and --user are required.\n", progname);
	    return false;	
	}
	else if (!dd_options->setCredential && (dd_options->hostsList || dd_options->user || dd_options->password ||
	                                        dd_options->log_level || dd_options->log_size))
	{
	    mpp_err_msg_cache(logError, progname, "The options --hostname and --user must follow the --setCredential option.\n", progname);	
	    return false;
	}
	else if (dd_options->setCredential && (dd_options->timestamp || dd_options->directory || dd_options->from_file || dd_options->to_file || 
									       dd_options->deleteDir || dd_options->deleteFile || dd_options->database || dd_options->syncFilesFromTimestamp ||
	                                       dd_options->listDir || dd_options->listDirFull || dd_options->copyFromDDBoost || dd_options->copyToDDBoost ||
	                                       dd_options->copy || dd_options->readFile || dd_options->getFreePercent || dd_options->getLatestTimestamp ||
	                                       dd_options->syncFiles || dd_options->writeToDDFileFromInput || dd_options->rename || dd_options->verify ||
	                                       dd_options->listSu || dd_options->createSu ))
	{
	    mpp_err_msg_cache(logError, progname, "The option --setCredential and its corresponding options are standalone."
	                                          "They shouldn't be used in conjunction with any other gpddboost option.\n");
	    return false;
	}
	
	return true;
}

/*
 * parmValNeedsQuotes: This function checks to see whether there is any whitespace in the parameter value.
 * This is used for pass thru parameters, top know whether to enclose them in quotes or not.
 */
static bool
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

static int
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
	ddp_stat_t stat_buf;
 

	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddBoostFileName;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"ddboost file %s open failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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
            mpp_err_msg(logError, progname, "failed to stat '%s'. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
            break;
        }
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

        err = ddp_read(handle, buf, rw_size,
                rw_size * i, &ret_count);
        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost read failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
            break;
        }
		
        written = fwrite(buf, 1, ret_count, fp);

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0));


cleanup:
    if (handle != DDP_INVALID_DESCRIPTOR)
        ddp_close_file(handle);
	if (buf)
		free(buf);

    return err;
}

static int
copyFileFromDDBoost(struct ddboost_options *dd_options)
{
	char *ddboostFile = Safe_strdup(dd_options->from_file);
	char *gpdbFile = Safe_strdup(dd_options->to_file);
	FILE *fp = NULL;
    int err = 0;

	if (!ddboostFile)
	{
		mpp_err_msg(logError, progname, "Source File on DDboost not specified\n");
		err = -1;
        goto cleanup;
	}

	if (!gpdbFile)
	{
		mpp_err_msg(logError, progname, "Destination file on GPDB not specified\n");
		err = -1;
        goto cleanup;
	}

	fp = fopen(gpdbFile, "w");
	if (fp == NULL)
	{
		mpp_err_msg(logError, progname, "Cannot open file %s on GPDB\n", gpdbFile);
		err = -1;
        goto cleanup;
	}

	/* Close ddboostFile in the called function */
	err = readFromDDFile(fp, ddboostFile);	

cleanup:
    if (gpdbFile)
        free(gpdbFile);
    if (fp)
        fclose(fp);
    if (ddboostFile)
        free(ddboostFile);

    return err;

}

static int
copyFileToDDBoost(struct ddboost_options *dd_options)
{
	char *gpdbFile = Safe_strdup(dd_options->from_file);
	char *ddBoostFile = Safe_strdup(dd_options->to_file);
	FILE *fp = NULL;
    int err = 0;

	if (!ddBoostFile)
	{
		mpp_err_msg(logError, progname, "Destination file on DDboost not specified\n");
		err = -1;
        goto cleanup;
	}

	if (!gpdbFile)
	{
		mpp_err_msg(logError, progname, "Source file on GPDB not specified\n");
		err = -1;
        goto cleanup;
	}

	fp = fopen(gpdbFile, "r");
	if (fp == NULL)
	{
		mpp_err_msg(logError, progname, "Cannot open file %s on GPDB\n", gpdbFile);
		err = -1;
        goto cleanup;
	}

	/* Close ddBoostFile in the called function */
	err = writeToDDFile(fp, ddBoostFile);

cleanup:
    if (gpdbFile)
        free(gpdbFile);

    if(ddBoostFile)
        free(ddBoostFile);

    if (fp)
        fclose(fp);

    return err;

}

static int
readFileFromDDBoost(struct ddboost_options *dd_options)
{
	char *ddBoostFile = Safe_strdup(dd_options->from_file);
    int err = 0;

	if (!ddBoostFile)
	{
		mpp_err_msg(logError, progname, "Destination file on DDboost not specified\n");
		return -1;
	}

	err = readFromDDFileToOutput(ddBoostFile);
    if (ddBoostFile)
        free(ddBoostFile);
    
    return err;

}

static int
deleteDir(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = dd_options->deleteDir;
    int err = 0;
	ddp_path_t path1 = {0};
	char full_path[MAX_PATH_NAME] = {0};


	sprintf(full_path, "%s", ddboostDir);

	path1.su_name = dd_conn_info.storageUnitName;
	path1.path_name = ddboostDir;

	err = ddBoostRmdir(ddboostDir, ddp_conn, ddboostDir);
	if (err)
	{
		mpp_err_msg(logError, progname, "dboost Deleting Directory %s failed with err %d\n", path1.path_name, err);
		err = -1;
	}

	return err;
}

static int
ddBoostRmdir(char *dir_path, ddp_conn_desc_t ddp_conn, char *parent_dir)
{
	ddp_path_t temp_path = {0};
	ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    dd_errno_t err = DD_ERR_NONE;
    ddp_dirent_t ret_dirent;
	char full_path[MAX_PATH_NAME] = {0};
	char cur_path[MAX_PATH_NAME] = {0};

	if (!dir_path)
		return 0;

	temp_path.path_name = dir_path;
	temp_path.su_name = dd_conn_info.storageUnitName;

	err = ddp_open_dir(ddp_conn, &temp_path, &dird);
	if (err)
	{
		mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", dir_path, ddp_errstr(err), err);
		return -1;
	}


	strcat(full_path, parent_dir);
	if (strcmp(&(full_path[strlen(full_path) - 1]), "/"))
		strcat(full_path, "/");

	/* Try to remove the directory. If the err is DD_ERR_NOTEMPTY then recursively delete the contents */
	err = ddp_rmdir(ddp_conn, &temp_path);
	if (err && err != DD_ERR_NOTEMPTY)
	{
		mpp_err_msg(logError, progname, "DDboost Directory %s deletion failed. Error: \"%s\" (%d)\n", temp_path.path_name, ddp_errstr(err), err);
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
			if ((strcmp(".", ret_dirent.d_name) == 0) ||
				(strcmp("..", ret_dirent.d_name) == 0))
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
                    mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed\n", cur_path);
                    err = -1;
					goto cleanup;
				}
					
			}	
			else if (err)
			{
				/* Deleting file failed with error */
                mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed. Error: \"%s\" (%d)\n", cur_path, ddp_errstr(err), err);
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
        mpp_err_msg(logError, progname, "ddboost Directory %s deletion failed. Error: \"%s\" (%d)\n", temp_path.path_name, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }
	else
	{
		/* Successfully deleted directory */	
		err = 0;
	}

cleanup:
    if (dird != DDP_INVALID_DESCRIPTOR)
        ddp_close_dir(dird);

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
	ddp_stat_t stat_buf;


	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddBoostFileName;

	err = createDDboostDir(ddp_conn, dd_conn_info.storageUnitName, ddBoostFileName);
	if (err)
	{
		mpp_err_msg(logError, progname, "Creating path %s on ddboost failed.\n", ddBoostFileName);
		err = -1;
		goto cleanup;			
	}

    err = ddp_open_file(ddp_conn, &path1, DDP_O_CREAT | DDP_O_RDWR, 0600, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"ddboost File %s open failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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

        err = ddp_write(handle, buf, read_bytes, rw_size * i, &ret_count);
        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost write failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
            err = -1;
            break;
        }

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0) && (rw_size == read_bytes));

	err = ddp_stat(ddp_conn, &path1, &stat_buf);
    if (err)
    {
        mpp_err_msg(logError, progname, "ddboost stat failed on %s failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    } 
		
	if (stat_buf.st_size < rw_size )
		rw_size = stat_buf.st_size;	

cleanup:
    if (handle != DDP_INVALID_DESCRIPTOR)
	    ddp_close_file(handle);

    /* Cleanup the file if copy failed */
    if (err)
        ddp_unlink(ddp_conn, &path1);
    if (buf)
        free(buf);

    return err;
}

static float getFreePercent(struct ddboost_options *dd_options)
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

static int
deleteFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	ddp_path_t temp_path = {0};
    dd_errno_t err = DD_ERR_NONE;

	if (!dd_options->deleteFile)
    {
        mpp_err_msg(logError, progname, "ddboost no file specified for delete\n");
		return -1;
    }

	temp_path.path_name = dd_options->deleteFile;
	temp_path.su_name = dd_conn_info.storageUnitName;
	
	err = ddp_unlink(ddp_conn, &temp_path);		
	if (err)
	{
        mpp_err_msg(logError, progname, "File %s cannot be deleted on DDboost. Error: \"%s\" (%d)\n", temp_path.path_name, ddp_errstr(err), err);
	}

	return err;
}

static int
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
	ddp_stat_t stat_buf;


	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddBoostFileName;
	
    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"Failed to open %s on ddboost. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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
               	mpp_err_msg(logError, progname, "ddboost stat failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
               	err = -1;
		        break;
        }
		
		if (stat_buf.st_size < rw_size )
			rw_size = stat_buf.st_size;	

        err = ddp_read(handle, buf, rw_size, rw_size * i, &ret_count);
        if (err)
        {
            mpp_err_msg(logError, progname, "ddboost read failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
            err = -1;
            break;
        }
	
		write(1, buf, ret_count);

        total_bytes += ret_count;
        i++;
    } while((err == 0) && (ret_count > 0));

cleanup:
    if (handle != DDP_INVALID_DESCRIPTOR)
        ddp_close_file(handle);
    if (buf)
        free(buf);

    return err;
}

static int
createDDboostDir(ddp_conn_desc_t ddp_conn, char *storage_unit_name, const char *filePath)
{
	char *pch = NULL;
    ddp_path_t path = {0};
    int err = 0;
    char full_path[MAX_PATH_NAME] = {0};
    /* We need to duplicate the string in order to use strtok() */
	char *path_name = Safe_strdup(filePath);

	if (!filePath)
	{
		mpp_err_msg(logError, progname, "Specify path for directory\n");
		err = -1;
        goto cleanup;
	}


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
                mpp_err_msg(logError, progname, "DDBoost mkdir failed. Error: \"%s\" (%d)\n", ddp_errstr(err), err);
				break;
            }

            strcat(full_path, "/");
            pch = strtok(NULL, " /");
		}
		else
			break;
    }

    /* It is not an error if the directory already exists */
    if (err == DD_ERR_EXIST)
        err = 0;

cleanup:
    if (path_name)
        free(path_name);        
    return err;
}

static int listDirectory(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
    char *ddboostDir = dd_options->directory;
    int err = 0;
    ddp_path_t path1 = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;


    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddboostDir;

	err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    printf("\nStorage Unit: '%s'\n", dd_conn_info.storageUnitName);
	while (1)
    {
        memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
        err = ddp_readdir(dird, &ret_dirent);
        if (err != DD_OK)
        {
            if (err == DD_ERR_UNDERFLOW)
			{
                err = NO_ERR;
			}
            else
			{
                mpp_err_msg(logError, progname, "Reading directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
			}

            /* We should exit the loop anyhow */
            break;
        }
        else
        {
            /* Skip deleting . and .. */
            if ((strcmp(".", ret_dirent.d_name) == 0) ||
            	(strcmp("..", ret_dirent.d_name) == 0))
            {
                continue;
            }

			printf("\n%s", ret_dirent.d_name);
		}
	}
	printf("\n");

cleanup:
    if (dird != DDP_INVALID_DESCRIPTOR)
        ddp_close_dir(dird); 
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
	ddp_stat_t stat_buf;
	int fd[2];
	char line[101];
	int done = false;
	char endString[MAX_PATH_NAME] = {0};
	int output, in_schema, found;
	char *curSchema = NULL;
	void *ddfp = NULL, *ddfpTemp = NULL;
	int isCompress = 0;

	if (!dd_options->to_file)
	{
        mpp_err_msg(logError, progname, "Output file not specified for selective restore\n");    
		return -1;
	}

	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = dd_options->from_file;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname,"Failed to open file %s on ddboost. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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

	isCompress = isFileCompressed(dd_options->from_file);

    /* Always try to create the Directory db_dumps and db_dumps/<date> before we start the
     * creation of fake dump files. If the directories already exist, then ignore the error.
     * This creation is important beacause, when we do a selective restore of an old backup 
     * onto a fresh cluster, the segment directories wont have the db_dumps and db_dumps/<date>
     * directories */
    err = createDbdumpsDir(dd_options->to_file);
    if (err)
    {
        mpp_err_msg(logError, progname, "Creating directory %s on ddboost failed", dd_options->to_file);
        err = -1;
        goto cleanup;
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
                mpp_err_msg(logError, progname, "ddboost stat failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
				break;
            }
		
			if (stat_buf.st_size < rw_size )
				rw_size = stat_buf.st_size;	

           	err = ddp_read(handle, buf, rw_size,
           	rw_size * i, &ret_count);
           	if (err)
            {
                mpp_err_msg(logError, progname, "ddboost read failed on %s. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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
            err = -1;
            goto cleanup;
        }

		ddfpTemp = DDOPEN(dd_options->to_file, "w", isCompress);
		if (ddfpTemp == NULL)
		{
            mpp_err_msg(logError, progname, "Opening file %s failed\n", dd_options->to_file);
			err= -1;
            goto cleanup;
		} 

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
                ddfp = NULL;
				DDCLOSE(ddfpTemp, isCompress);	
                ddfpTemp = NULL;	
				done = true;
			}
		} while (!done);
		close(fd[0]);
	}

cleanup:
    if (handle != DDP_INVALID_DESCRIPTOR)
        ddp_close_file(handle);
    if (buf)
	    free(buf);	
    if (curSchema)
        free(curSchema);
    if (ddfp)
        DDCLOSE(ddfp, isCompress);
    if (ddfpTemp)
        DDCLOSE(ddfpTemp, isCompress);
	return 0;
}

/* check if the line starts with the specified pattern */
static int
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
static int
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
static int
lineEndsWith(char *line, char *pattern)
{
	if (strstr(line, pattern))
		return 1;
	else
		return 0;

}
	 
static int
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
	char *ddboostDir = dd_options->directory;
    int err = 0;
    ddp_path_t path1 = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;
	char recentTimestamp[MAX_PATH_NAME] = {0};
	char filePath[MAX_PATH_NAME] = {0};
	int found  = 0;


    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddboostDir;

	err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Directory %s open failed on ddboost. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
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
            }
            else
            {
                mpp_err_msg(logError, progname, "ddboost readdir on %s failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
            }
			
			/* We should exit the loop anyhow */
			break;
        }

        else
        {
            /* Skip . and .. */
        	if ((strcmp(".", ret_dirent.d_name) == 0) ||
				(strcmp("..", ret_dirent.d_name) == 0))
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

cleanup:
		
	return err;
}


static int
dumpFileHasDatabaseName(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *fullPath, char *databaseName)
{
	/* Read the first 5 lines of the cdatabase file on DDboost */
    ddp_file_desc_t handle = DDP_INVALID_DESCRIPTOR;
    int err = 0, ret = 0;
    ddp_uint64_t ret_count = 0;
    char buf[MAX_PATH_NAME+1] = {0};
    ddp_uint64_t rw_size = dd_boost_buf_size;
	ddp_path_t path1 = {0};
	ddp_stat_t stat_buf;
	char searchString[MAX_PATH_NAME] = {0};


	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = fullPath;

    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ , 0400, &handle);
    if (err)
    {
        mpp_err_msg(logError, progname, "ddboost file  %s open failed. Error: \"%s\" (%d)\n", fullPath, ddp_errstr(err), err);
    	ret = 0;
		goto cleanup;
    }


	/* The CREATE DATABASE statement is usually in the first 5 lines */
    ret_count = 0;
		
	err = ddp_stat(ddp_conn, &path1, &stat_buf);
	if (err)
	{
		ret = 0;
        mpp_err_msg(logError, progname, "DDboost Stat on file %s failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
		goto cleanup;
	}	
		
	if (stat_buf.st_size < MAX_PATH_NAME )
		rw_size = stat_buf.st_size;	

    err = ddp_read(handle, buf, MAX_PATH_NAME, 0, &ret_count);

    if (err)
	{
        mpp_err_msg(logError, progname, "Reading file %s on ddboost failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
        ret = 0;
		goto cleanup;
	}
	

	sprintf(searchString, "CREATE DATABASE %s", databaseName);

	if (strstr(buf, searchString))
		ret = 1;

cleanup:
	
	return ret;
}

/* Copy over files from DDboost to GPDB master segment directory */
static int
syncFilesFromDDBoost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath)
{
	char *ddboostDir = "db_dumps";
    int err = 0;
    ddp_path_t path1 = {0};
    char full_path[MAX_PATH_NAME] = {0};
	char dest_path[MAX_PATH_NAME] = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;


    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddboostDir;

	/* Start traversing from db_dumps/ directory */
	err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
        err = -1;
		goto cleanup;
    }

	/* Create the db_dumps directory if it doesn't exist already */
	sprintf(dest_path, "%s/%s", dirPath, ddboostDir); 
	
	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if (err && (err != EEXIST) && (err != -1))
	{
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed\n", dest_path);
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
                mpp_err_msg(logError, progname, "ddboost readdir failed on %s. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
                goto cleanup;
            }
        }
        else
        {
        	if ((strcmp(".", ret_dirent.d_name) == 0) ||
				(strcmp("..", ret_dirent.d_name) == 0))
			{
				continue;
			}

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

	return err;
}


static int
copyFilesFromDir(const char *fromDir, char *toDir, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = (char*)fromDir;
    int err = 0;
    ddp_path_t path1 = {0};
    char full_path[MAX_PATH_NAME] = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;
	char dest_path[MAX_PATH_NAME] = {0};
	FILE *fp = NULL;

    sprintf(full_path, "%s", ddboostDir);
	sprintf(dest_path, "%s/%s", toDir, fromDir); 

	path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddboostDir;

	/* Start traversing from db_dumps/ directory */
	err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
		err = -1;
		goto cleanup;
    }

	/* Create the directory on the destination if it doesn't exist */
	err = mkdir(dest_path, S_IRWXU);
	if (err && (err != EEXIST) && (err != -1))
	{
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed.\n", dest_path);
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
                mpp_err_msg(logError, progname, "ddboost read dir failed on %s. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
                goto cleanup;
            }
        }
        else
        {
            if ((strcmp(".", ret_dirent.d_name) == 0) ||
            	(strcmp("..", ret_dirent.d_name) == 0))
            {
                continue;
            }

			if (!isFileToBeCopied(ret_dirent.d_name))
			{
				continue;
			}

	 		/* For every date, copy the required files over to GPDB */
			sprintf(full_path, "%s/%s", ddboostDir, ret_dirent.d_name);
			sprintf(dest_path, "%s/%s", toDir, full_path);

			fp = fopen(dest_path, "w");
			if (!fp)
			{
                mpp_err_msg(logError, progname, "File %s open failed on GPDB\n", dest_path);
				err = -1;
				break;
			}

            /* File full_path is closed in the callee */
			err = readFromDDFile(fp, full_path);
		}
	}

cleanup:
    if (fp)
        fclose(fp);

	return err;
}

static int
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


	path1.su_name = dd_conn_info.storageUnitName;
	path1.path_name = ddBoostFileName;

	err = createDDboostDir(ddp_conn, dd_conn_info.storageUnitName, ddBoostFileName);
	if (err)
	{
		mpp_err_msg(logError, progname, "ddboost Creating path %s failed\n", ddBoostFileName);
		err = -1;
		goto cleanup;	
	}

	err = ddp_open_file(ddp_conn, &path1, DDP_O_CREAT | DDP_O_RDWR, 0600, &handle);
	if (err)
	{
		mpp_err_msg(logError, progname,"ddboost File %s open failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
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
		err = -1;
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
                        mpp_err_msg(logError, progname,"ddboost File %s write failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
						break;
                    }
					total_bytes += ret_count;
					memset(buf_iogroup, 0, rw_size);
					buf_data_length = 0;
				}


				err = ddp_write(handle, buf, read_bytes, total_bytes, &ret_count);
				if (err)
                {
                    mpp_err_msg(logError, progname,"ddboost File %s write failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
					err = -1;
                    goto cleanup;
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
					err = ddp_write(handle, buf_iogroup, buf_data_length, total_bytes, &ret_count);
					if (err)
                    {
                        mpp_err_msg(logError, progname,"ddboost File %s write failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
						err = -1;
                        goto cleanup;
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
		mpp_err_msg(logError, progname, "ddboost File %s write failed. Error: \"%s\" (%d)\n", path1.path_name, ddp_errstr(err), err);
		err = -1;
	}

cleanup:
	if (buf)
		free(buf);
	if (buf_iogroup)
		free(buf_iogroup);

    if (handle != DDP_INVALID_DESCRIPTOR)
	    ddp_close_file(handle);
	return err;
}

static int
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

static int
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
                    mpp_err_msg(logError, progname,"Directory %s creation on GPDB path failed", curPath);
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

static int listDirectoryFull(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	char *ddboostDir = dd_options->directory;
    int err = 0;
    ddp_path_t dd_path;
    char current_path[MAX_PATH_NAME] = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;
    ddp_stat_t stat_buf;
    ddp_file_desc_t file_descriptor = DDP_INVALID_DESCRIPTOR;

    dd_path.su_name = dd_conn_info.storageUnitName;
    dd_path.path_name = ddboostDir;

	err = ddp_open_dir(ddp_conn, &dd_path, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    mpp_err_msg(logInfo, progname, "\n");
    mpp_err_msg(logInfo, progname, "Storage Unit: '%s'\n", dd_conn_info.storageUnitName);
    mpp_err_msg(logInfo, progname, "\n");
    mpp_err_msg(logInfo, progname, "%20s%23s\t\t\t\t%s\t%s\n", "NAME", "", "MODE", "SIZE(bytes)");
    mpp_err_msg(logInfo, progname, "===============================================================================================\n");

    /* Loop over all the contents in the directory */
	while (1)
    {
        memset(&ret_dirent, 0, sizeof(ddp_dirent_t));
        err = ddp_readdir(dird, &ret_dirent);
        if (err != DD_OK)
        {
        	/* Check if the end of the directory has been reached*/
            if (err == DD_ERR_UNDERFLOW)
			{
                err = NO_ERR;
			}
            else
			{
                mpp_err_msg(logError, progname, "Reading directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
			}

            /* We should exit the loop anyhow */
			break;
        }
        else
        {
            /* Skip . and .. */
            if ((strcmp(".", ret_dirent.d_name) == 0) ||
            	(strcmp("..", ret_dirent.d_name) == 0))
            {
                continue;
            }

            /* "Create" the path of the current file\directory */
			sprintf(current_path, "%s/%s", ddboostDir, ret_dirent.d_name);

            /* There is no need to set the storage unit again - it was already set */
            dd_path.path_name = current_path;

            /* Open the file\directory */
            err = ddp_open_file(ddp_conn, &dd_path, DDP_O_READ, 0, &file_descriptor);
            if (err)
            {
                mpp_err_msg(logError, progname, "Reading file %s on ddboost failed. Error: \"%s\" (%d)\n", current_path, ddp_errstr(err), err);
                break;
            }

            /* Get the status of the file\directory */
            err = ddp_fstat(file_descriptor, &stat_buf);
            if (err)
            {
                mpp_err_msg(logError, progname, "Stat on file %s on ddboost failed. Error: \"%s\" (%d)\n", current_path, ddp_errstr(err), err);
                ddp_close_file(file_descriptor);
                break;
            }

            /* If this is a directory - add 'D' to its mode */
            if (S_ISDIR(stat_buf.st_mode))
            	mpp_err_msg(logInfo, progname, "%-50s\t\t\tD%o\t%llu\n", ret_dirent.d_name, (stat_buf.st_mode & 07777), stat_buf.st_size);
            else
            	mpp_err_msg(logInfo, progname, "%-50s\t\t\t%o\t%llu\n", ret_dirent.d_name, (stat_buf.st_mode & 07777), stat_buf.st_size);

            ddp_close_file(file_descriptor);
		}
	}
	mpp_err_msg(logInfo, progname, "\n");

cleanup:
    if (dird != DDP_INVALID_DESCRIPTOR)
        ddp_close_dir(dird);

	return err;
}

/* Copy over files from DDboost to GPDB master segment directory */
static int
syncFilesFromDDBoostTimestamp(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn, char *dirPath)
{
    char *ddboostDir = "db_dumps";
    int err = 0;
    ddp_path_t path1 = {0};
    char ddboostPath[MAX_PATH_NAME] = {0};
    char gpdbPath[MAX_PATH_NAME] = {0};
    ddp_dir_desc_t dird = DDP_INVALID_DESCRIPTOR;
    ddp_dirent_t ret_dirent;
    char tempDate[10];
    FILE *fp = NULL;

    if (!dd_options->syncFilesFromTimestamp)
    {
        mpp_err_msg(logError, progname, "Timestamp not specified for sync\n");
        err = -1;
        goto cleanup;
    }

    /*  Form Directory path from the timestamp. The first 8 characters of the timestamp
        is of the form YYYYMMDD and we need to go to the directory db_dumps/YYYYMMDD.
        Then we search for the file whcih has the <timestamp> in its name.
    */
    memset(tempDate, 0, 10);
    snprintf(tempDate, 9, "%s", dd_options->syncFilesFromTimestamp);

    sprintf(ddboostPath, "%s/%s", ddboostDir, tempDate);

    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = ddboostPath;

    /* Start traversing the db_dumps/<date> directory */
    err = ddp_open_dir(ddp_conn, &path1, &dird);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening directory %s on ddboost failed. Error: \"%s\" (%d)\n", ddboostPath, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    /* Create the db_dumps directory if it doesn't exist already */
    sprintf(gpdbPath, "%s/%s", dirPath, ddboostDir); 
	
    /* Create the db_dumps directory on the destination if it doesn't exist */
    err = mkdir(gpdbPath, S_IRWXU);
    if (err && (err != EEXIST) && (err != -1))
    {
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed", gpdbPath);
        err = -1;
        goto cleanup;	
    }

    /* Create the destination directory on the destination if it doesn't exist */
    memset(gpdbPath, 0, MAX_PATH_NAME);
    sprintf(gpdbPath, "%s/%s/%s", dirPath, ddboostDir, tempDate);
    err = mkdir(gpdbPath, S_IRWXU);
    if (err && (err != EEXIST) && (err != -1))
    {
        mpp_err_msg(logError, progname, "Directory %s creation on GPDB path failed", gpdbPath);
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
                mpp_err_msg(logError, progname, "ddboost readdir failed on %s. Error: \"%s\" (%d)\n", ddboostDir, ddp_errstr(err), err);
                goto cleanup;
            }
        }
        else
        {
            /* Only check directories of the form YYYYMMDD */
            if (!strstr(ret_dirent.d_name, dd_options->syncFilesFromTimestamp))
                continue;

            /* For every matching file , copy the required files over to GPDB */
            memset(ddboostPath, 0, MAX_PATH_NAME);
            sprintf(ddboostPath, "%s/%s/%s", ddboostDir, tempDate, ret_dirent.d_name);  

            memset(gpdbPath, 0, MAX_PATH_NAME);
            sprintf(gpdbPath, "%s/%s/%s/%s", dirPath, ddboostDir, tempDate, ret_dirent.d_name);
            fp = fopen(gpdbPath, "w");
            if (fp == NULL)
            {
                mpp_err_msg(logError, progname, "Cannot open file %s on GPDB\n", gpdbPath);
                err = -1;
                break;
            }

            /* Close both files in the called function */
            err = readFromDDFile(fp, ddboostPath);  
            if (err)
            {
                mpp_err_msg(logError, progname, "Copy failed from DDboost file %s to GPDB file %s\n", ddboostPath, gpdbPath);
                break;
            }      

        }
    }

cleanup:

	return err;
}

static int
setCredential(struct ddboost_options *dd_options)
{
	/* take password - getpass works only on linux */
	if (!dd_options->password)
	{
		dd_options->password = getpass("Password: ");
	}

	//
	// Placeholder for future enhancement
	//
	// Adapt setDDBoostCredential() to receive a list, rather than one host 'dd_options->hostsList->hostName'
	//

	if (setDDBoostCredential(dd_options->hostsList->hostName, dd_options->user, dd_options->password,
	                         dd_options->log_level, dd_options->log_size) < 0)
		return -1;

	return 0;
}

static int
renameFile(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
    char *fromFile = dd_options->from_file;
    char *toFile = dd_options->to_file;
    int err = 0;
    ddp_path_t path1 = {0};
    ddp_path_t path2 = {0};

    if (!fromFile)
    {
        mpp_err_msg(logError, progname, "Source file on DDboost not specified\n");
        return -1;
    }

    if (!toFile)
    {
        mpp_err_msg(logError, progname, "Destination file on GPDB not specified\n");
        err = -1;
        goto cleanup;
    }

    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = fromFile;

    path2.su_name = dd_conn_info.storageUnitName;
    path2.path_name = toFile;

    /* Create a directory on the destination if it doesn't exist */
    err = createDDboostDir(ddp_conn, dd_conn_info.storageUnitName, toFile);
    if (err)
    {
        mpp_err_msg(logError, progname, "Creating path %s on ddboost failed", fromFile);
        err = -1;
        goto cleanup;			
    }

    /* Now try to rename the original file to the new name */
    err = ddp_rename(ddp_conn, &path1, &path2);
    if (err)
    {
        mpp_err_msg(logError, progname,"ddboost File %s rename to %s failed. Error: \"%s\" (%d)\n", path1.path_name, path2.path_name, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

cleanup:

    return 0;
}

static int
copyWithinDDboost(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
    char *fromFile = dd_options->from_file;
    char *toFile = dd_options->to_file;
    int err = 0;
    ddp_path_t path1;
    ddp_path_t path2;
    ddp_file_desc_t src_fh = DDP_INVALID_DESCRIPTOR;
    ddp_file_desc_t dst_fh = DDP_INVALID_DESCRIPTOR;
    ddp_uint64_t done_offset = 0;
    ddp_uint64_t bytescopied = 0;
    ddp_stat_t stat_buf;


    path1.su_name = dd_conn_info.storageUnitName;
    path1.path_name = fromFile;

    path2.su_name = dd_conn_info.storageUnitName;
    path2.path_name = toFile;

    /* Create a directory on the destination if it doesn't exist */
    err = createDDboostDir(ddp_conn, dd_conn_info.storageUnitName, toFile);
    if (err)
    {
        mpp_err_msg(logError, progname, "Creating path %s on ddboost failed. Err %d\n", fromFile, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    /* Open the file on source before we start the copy */
    err = ddp_open_file(ddp_conn, &path1, DDP_O_READ, 0400, &src_fh);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening file %s on ddboost failed. Error: \"%s\" (%d)\n", fromFile, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    /* Open the file on destination before we start the copy */
    err = ddp_open_file(ddp_conn, &path2, DDP_O_CREAT | DDP_O_RDWR, 0644, &dst_fh);
    if (err)
    {
        mpp_err_msg(logError, progname, "Opening file %s on ddboost failed. Error: \"%s\" (%d)\n", toFile, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    /* Get the size of the source file to be copied */
    err = ddp_stat(ddp_conn, &path1, &stat_buf);
    if (err)
    {
        mpp_err_msg(logError, progname, "Stat on ddboost file %s failed. Error: \"%s\" (%d)\n", fromFile, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }

    /* Now start the copy Before we start the filecopy, try to stop it *
     * It should not be found DDFS should exit with no error *
     */
    ddp_filecopy_stop(ddp_conn, dst_fh);
    
    /* Start the file copy */
    err = ddp_filecopy_start(src_fh, dst_fh, NULL);
    if (err)
    {
        mpp_err_msg(logError, progname, "File copy failed to start on DDboost. Src file %s Dest file %s. Error: \"%s\" (%d)\n",
                    fromFile, toFile, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }     
    
    done_offset = 0;
    while (done_offset != stat_buf.st_size) 
    {
        bytescopied = 0;
        err = ddp_filecopy_status(src_fh, dst_fh,
                                   done_offset,
                                   DDP_FILECOPY_EXTENT_LEN,
                                   &bytescopied);
        if (err != DD_OK) 
        {
            ddp_filecopy_stop(ddp_conn, dst_fh);
            mpp_err_msg(logError, progname, "File copy on ddboost failed. Src file %s Dest file %s. Error: \"%s\" (%d)\n",
                        fromFile, toFile, ddp_errstr(err), err);
            err = -1;
            goto cleanup;
        }
        done_offset += bytescopied;
    }

    /* Check if all the data has been copied */
    if (done_offset != stat_buf.st_size) 
    {
        ddp_filecopy_stop(ddp_conn, dst_fh);
        mpp_err_msg(logError, progname, "File copy on ddboost failed to complete. Src file %s Dest file %s"
                                        "Total size %lld completed size %lld. Error: \"%s\" (%d)\n",
                    fromFile, toFile, stat_buf.st_size, done_offset, ddp_errstr(err), err);
        err = -1;
        goto cleanup;
    }
    
    ddp_filecopy_stop(ddp_conn, dst_fh);

cleanup:
    if (src_fh != DDP_INVALID_DESCRIPTOR)
        ddp_close_file(src_fh);
    if(dst_fh != DDP_INVALID_DESCRIPTOR)
        ddp_close_file(dst_fh);
    /* If err then delete the partially copied file */
    if (err)
    {
        ddp_unlink(ddp_conn, &path2); 
    }
        
    return err;
}


/*
 * This function List all the available storage units on all the available Data Domain appliances
 * Returns 0 in case of success
 */
static int listAllStorageUnits(ddp_conn_desc_t ddp_conn)
{
	int				result = 1;
	int				i=0;
	ddp_su_list_t 	su_list;


	/* Allocate space for the storage unit names list 		*/
	/* The memory is released at the end of this function	*/
	su_list.su_names_buf = (ddp_su_name_info_t*)ddMalloc(DDP_MAX_NUMBER_OF_STORAGE_UNITS * sizeof(ddp_su_name_info_t));
	if (su_list.su_names_buf == NULL)
		return 1;

	//
	// This is a placeholder for future enhancements
	//
	// We will add an option to connect to more that one DDR.
	// Hence we will need to iterate over all the available DD_hosts and print all the storage units.
	// To avoid duplicate printing, we can determine that different DD_hosts are actually a part of
	// the same DD server by using ddp_get_system_info(), and then comparing the serial number of the DD servers.
	//

	/* Indicate how much memory we actually allocated */
	su_list.su_count = DDP_MAX_NUMBER_OF_STORAGE_UNITS;

	/* Get all storage units from this host */
	if (getStorageUnitsListFromOneHost(dd_conn_info.hostname, &su_list) == NO_ERR)
	{
		/* Loop over all the storage units and print them 		*/
		/* su_list looks as follows:							*/
		/*	-----------------	    				        	*/
		/*	|  su_count	 	|	 ----------------------- 		*/
		/*	|  su_names_buf	---> | name[50] | name[50] | ...	*/
		/*	-----------------	 -----------------------		*/
		/*														*/
		char *current_su;
		mpp_err_msg(logInfo, progname, "There are %d storage units on host '%s'\n", su_list.su_count, dd_conn_info.hostname);
		for (i=0; i<su_list.su_count; i++)
		{
			/* Point to the current storage unit */
			current_su = (su_list.su_names_buf+i)->name;
			mpp_err_msg(logInfo, progname, "[%02d] %s\n", i+1, current_su);
		}

		result = NO_ERR;
	}

	/* The memory was allocated at the beginning of this function */
	free(su_list.su_names_buf);
	return result;
}


/* This function connects to a single DD servers, and receives all storage units */
static int getStorageUnitsListFromOneHost(char *hostName, ddp_su_list_t *su_list)
{
	dd_errno_t	result;

	//
	// This is a placeholder for future enhancements
	//
	// We will need to see if there is existing connection to this host (hostName),
	// if not, open a new one.
	//

	/* Get the list from DD server */
	result = ddp_get_storage_units_list(ddp_conn, su_list);
	if (result == DD_OK)
	{
		return NO_ERR;
	}
	else
	{
		mpp_err_msg(logError, progname, "Couldn't retrieve the storage units list from '%s'. Error: \"%s\" (%d)\n", hostName, ddp_errstr(result), result);
		return 1;
	}
}


/* This function creates a storage unit - 'su_name' on all available DD_hosts */
static int createStorageUnitOnAllHosts(ddp_conn_desc_t ddp_conn, const char *su_name)
{
	dd_errno_t	result;

	//
	// Placeholder
	//
	// CHECK THAT THIS SU DOESN'T EXIST
	//
	// [1] Iterate over all existing DD_hosts
	//
	// [2] if doesn't exist - create
	//
	//
	result = ddp_create_storage_unit(ddp_conn, su_name);
	if (result != DD_OK)
	{
		mpp_err_msg(logError, progname, "Failed to create storage unit \"%s\" on host '%s'. Error: \"%s\" (%d)\n",
					su_name, dd_conn_info.hostname, ddp_errstr(result), result);
		return 1;
	}

	/* After the storage unit was created, save it to the Lockbox 	*/
	/* We update it (rather than create is), since it was already	*/
	/* created in setDDBoostCredentials()							*/
	return updateLbItem(LB_ITEM_NAME_STORAGE_UNIT_NAME, su_name);

}


/* This functions creates the current SU on all DD hosts 						*/
/* We assume that the current SU name was already set in setDDBoostCredential()	*/
static int createCurrentStorageUnitOnAllHosts(struct ddboost_options *dd_options, ddp_conn_desc_t ddp_conn)
{
	int			result;
	char 		*currentSuName = NULL;

	/* Get current SU name */
	if (getExistingLbItems(&currentSuName))
		return 1;

	/* Create the SU on all hosts */
	result = createStorageUnitOnAllHosts(ddp_conn, currentSuName);
	return result;
}


/* Add new hostname to the hostNameList 				*/
/* The new host is added at the beginning of the list 	*/
static int addNewHostName(struct ddboost_options *dd_options, char *hostName)
{
	/* Allocate memory for the new new host */
	hostNameList *newHostPtr = ddMalloc(sizeof(hostNameList));
	if (newHostPtr == NULL)
		return 1;

	newHostPtr->hostName = Safe_strdup(hostName);

	/* Add the new host to the beginning of the list */
	newHostPtr->next = dd_options->hostsList;
	dd_options->hostsList = newHostPtr;

	return 0;
}


/* Free dd_options members */
static void freeDdOptions(struct ddboost_options *dd_options)
{
	if (dd_options->createSu != NULL)
		free(dd_options->createSu);

	if (dd_options->hostsList != NULL)
	{
		//TODO: Free all the list
		if (dd_options->hostsList->hostName)
			free(dd_options->hostsList->hostName);

		free(dd_options->hostsList);
	}
}


/* This function safely allocates memory of size 'size' */
/* Prints error message in case of failure				*/
static void *ddMalloc(size_t size)
{
	void *result = malloc(size);
	if ( result != NULL )
	{
		return result;
	}
	else
	{
		mpp_err_msg(logError, progname, "Memory allocation failed\n");
		return NULL;
	}
}


#else
int main(int argc, char **argv)
{
	mpp_err_msg(logError, progname,"\n");
	mpp_err_msg(logError, progname,"gpddboost is not supported on this platform\n");
	mpp_err_msg(logError, progname,"\n");
	return 0;
}	
#endif
