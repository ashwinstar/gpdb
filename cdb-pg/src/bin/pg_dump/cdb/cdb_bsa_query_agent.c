#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "pg_config.h"
#include "fe-auth.h"
#include "cdb_dump_util.h"
#include "getopt_long.h"

#ifdef USE_NETBACKUP

#include "cdb_bsa_util.h"

char                    *BackupFilePathName;
int						BackupFilePathLength = 0;
char                    *NetBackupServiceHost;
char					*progname;
char					*netbackupRestoreFilename = NULL;
char					*netbackupServiceHost = NULL;
extern char				*optarg;
extern int				optind;

static void help(const char *progname);

int
main(int argc, char *argv[])
{
	int c;

	static struct option long_options[] = {
		{"netbackup-service-host", required_argument, NULL, 1},
		{"netbackup-filename", required_argument, NULL, 2},
		{NULL, 0, NULL, 0}
	};

	int optindex;
	progname = (char *)get_progname(argv[0]);

	if (argc > 1)
	{
		if ((strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "-?") == 0))
		{
			help(progname);
			exit(0);
		}
		if ((strcmp(argv[1], "--version") == 0) || (strcmp(argv[1], "-V") == 0))
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while((c = getopt_long(argc, argv, "12", long_options, &optindex)) != -1)
	{
		switch(c)
		{
			case 1:
				netbackupServiceHost = strdup(optarg);
				break;
			case 2:
				netbackupRestoreFilename = strdup(optarg);
				break;
			default:
				fprintf(stderr, _("Wrong option entered. Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: Too many command-line arguments (first is \"%s\")\n"), progname, argv[optind + 1]);
		fprintf(stderr, _("Try \"%s --help\" for more information\n"), progname);
		exit(1);
	}

	if (netbackupServiceHost == NULL)
	{
		mpp_err_msg("ERROR", "gp_bsa_dump_agent", "Need to provide --netbackup-service-host mandatory option.\nTry \"%s --help\" for more information\n", progname);
		exit(1);
	}

	if (netbackupRestoreFilename == NULL)
	{
		mpp_err_msg("ERROR", "gp_bsa_dump_agent", "Need to provide NetBackup Dump File Path.\nTry \"%s --help\" for more information\n", progname);
		exit(1);
	}

	BackupFilePathLength = strlen(netbackupRestoreFilename);
	BackupFilePathName = (char *)malloc(sizeof(char) *(1 + BackupFilePathLength));
	if(BackupFilePathName == NULL){
		mpp_err_msg("ERROR", "gp_bsa_query_agent", "Failed to allocate memory for Restore Filename\n");
		exit(1);
	}
	strncpy(BackupFilePathName, netbackupRestoreFilename, (1 + BackupFilePathLength));

	NetBackupServiceHost = (char *)malloc(sizeof(char) *(1 + strlen(netbackupServiceHost)));
	if(NetBackupServiceHost == NULL){
		mpp_err_msg("ERROR", "gp_bsa_dump_agent", "Failed to allocate memory for NetBackup Service Hostname\n");
		exit(1);
	}
	strncpy(NetBackupServiceHost, netbackupServiceHost, (1 + strlen(netbackupServiceHost)));


	if(initBSARestoreSession(NetBackupServiceHost) != 0){
		mpp_err_msg("ERROR", "gp_bsa_query_agent", "Failed to initialize the NetBackup BSA session to query BSA object\n");
		exit(1);
	}

	if(queryBSAObject(BackupFilePathName) == NULL){
		mpp_err_msg("INFO", "gp_bsa_query_agent", "Query to NetBackup server failed for object: %s\n", BackupFilePathName);
	}
	else {
		printf("%s\n", (char *)BackupFilePathName);
	}

	exit(0);

}

static void
help(const char *progname)
{
	printf(_("\n%s queries the given object to NetBackup server to determine if it has been backup up using NetBackup\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("	%s [OPTION]... [FILENAME]"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  --netbackup-service-host=HOSTNAME  \n"));
	printf(_("  NetBackup master server hostname.\n"));
	printf(_("  NetBackup master server hostname should be provided to query the NetBackup server.\n\n\n"));
	printf(_("  --netbackup-filename=FILENAME  \n"));
	printf(_("  NetBackup filename to be queried.\n"));
	printf(_("  NetBackup filename should be provided to query the NetBackup server.\n\n\n"));

	printf(_("\nIf no file name is provided the utility will fail.\n"));
}

#else
int main(int argc, char **argv)
{
	mpp_err_msg("INFO", "gp_bsa_query_agent", "\ngp_bsa_query_agent is not supported on this platform\n\n");
	exit(0);
}

#endif
