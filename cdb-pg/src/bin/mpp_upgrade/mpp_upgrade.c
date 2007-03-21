#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "postgres_fe.h"
#include "libpq-fe.h"

/*typedef int bool;*/

/*
 * Normally, it's not possible to create shared (pg_global) tables and indexes after initdb, because
 * the system doesn't know how to upgrade the catalogs of all the databases.
 * 
 * This utility does it by cycling through each database, but to get this to work, we
 * needed custom changes to the postgres code to allow these DDL statements.
 */

static char * pg_resqueue = 
	"CREATE TABLE pg_catalog.pg_resqueue (rsqname name, rsqcountlimit float4, rsqcostlimit float4, rsqovercommit bool) "
	" WITH OIDS TABLESPACE pg_global";
	
static char * pg_resqueue_index1 = 
	"CREATE UNIQUE INDEX pg_resqueue_oid_index ON pg_catalog.pg_resqueue(oid)";
static char * pg_resqueue_index2 = 
	"CREATE UNIQUE INDEX pg_resqueue_rsqname_index ON pg_catalog.pg_resqueue(rsqname)";


static char * pg_authid = "ALTER TABLE pg_catalog.pg_authid ADD rolresqueue oid";

static char * pg_authid_index1 = 
	"CREATE UNIQUE INDEX pg_authid_rolresqueue_index ON pg_catalog.pg_authid(rolresqueue)";

/*
 * Damn... Can't REPLACE a view with a new definition that has a different number of columns.
 * Why????  We have to DROP the old definition, and CREATE the new one.
 */

static char * pg_locks = 
   "CREATE VIEW pg_catalog.pg_locks AS "
   "SELECT * "
   "FROM pg_lock_status() AS L "
   "(locktype text, database oid, relation oid, page int4, tuple int2,"
   "  transactionid xid, classid oid, objid oid, objsubid int2,"
    " transaction xid, pid int4, mode text, granted boolean, mppSessionId int4, mppIsWriter boolean)";
     


static char * pg_roles = 
	"CREATE VIEW pg_catalog.pg_roles AS "
	"SELECT pg_authid.rolname, pg_authid.rolsuper, pg_authid.rolinherit, pg_authid.rolcreaterole, pg_authid.rolcreatedb, pg_authid.rolcatupdate, pg_authid.rolcanlogin, pg_authid.rolconnlimit, '********'::text AS rolpassword, pg_authid.rolvaliduntil, pg_authid.rolconfig, pg_authid.rolresqueue, pg_authid.oid FROM pg_authid";
   
   
static char * newconfinfo =
"\n"
"#---------------------------------------------------------------------------\n"
"# RESOURCE SCHEDULING\n"
"#---------------------------------------------------------------------------\n"
"\n"
"#resource_scheduler = off       # enable resource scheduling\n"
"#max_resource_queues = 8        # no. of resource queues to create.\n"
"#max_resource_portals_per_transaction = 64  # no. of portals per backend.\n"
"#resource_select_only = on      # resource lock SELECT queries only.\n\n";

   
static bool verbose = false;
 
static void MyNoticeProcessor (void *arg, const char *message)
{
	fprintf(stderr,"%s",message);
} 


static PGconn     *
connect_to_db(char * conninfo, bool require_password)
{
	PGconn     *conn;
    PGresult   *res;
    bool		need_pass = false;
	static char *password = NULL;
    
    if (require_password && !password)
		password = simple_prompt("Password: ", 100, false);
	
	do
	{
		need_pass = false;
		
	    /* Make a connection to the database */
	    
	    if (password)
	    {
	     	strcat(conninfo," password=");
	     	strcat(conninfo,password);
	    }
	    	
	    conn = PQconnectdb(conninfo);

	   
    
    	if (PQstatus(conn) == CONNECTION_BAD &&
			strcmp(PQerrorMessage(conn), PQnoPasswordSupplied) == 0 &&
			!feof(stdin))
		{
			PQfinish(conn);
			need_pass = true;
			if (password)
				free(password);
			password = NULL;
			password = simple_prompt("Password: ", 100, false);
		}
	} while (need_pass);
	
	 /* Check to see that the backend connection was successfully made */
    if (PQstatus(conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Connection to database failed: %s\n  Using conninfo %s\n",
                PQerrorMessage(conn),conninfo);
        PQfinish(conn);
    	exit(1);
    }
    
    PQsetNoticeProcessor(conn,
                     MyNoticeProcessor,
                     NULL);
    
	res = PQexec(conn, "set search_path='pg_catalog'");
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "SET SEARCH_PATH failed: %s", PQerrorMessage(conn));
        PQclear(res);
       	PQfinish(conn);
    	exit(1);
    }
    PQclear(res);
    return conn;
}

static void run_sql_command(PGconn * conn, char * cmd)
{
	PGresult   *res;
	char		SqlState[6];
	
	if (verbose)
		printf("%s\n",cmd);
	res = PQexec(conn, cmd);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
    	 SqlState[0] = '\0';
         if (PQresultErrorField(res, PG_DIAG_SQLSTATE) != NULL)
            strcpy(SqlState, PQresultErrorField(res, PG_DIAG_SQLSTATE));
            
    	if (strcmp(SqlState,"42P07")!=0 &&  /* Table already exists */
    	    strcmp(SqlState,"42701")!=0)	/* Column already exists */
    	{
	        fprintf(stderr, " %s: command failed: \n%s %s\n", cmd,SqlState, PQerrorMessage(conn));
	        PQclear(res);
	        PQfinish(conn);
	        fprintf(stderr," UPGRADE FAILED\n");
	    	exit(1);
    	}
    	PQclear(res);
    }
    else
    	PQclear(res);
	
}

static void
help(void)
{
	printf("gp_upgrade is used to upgrade a Greenplum (Bizgres MPP) 2.2 system to a Greenplum Database 2.3 system.\n\n");
	printf("gp_upgrade must be run from the master database node after upgrading the Greenplum Database software.\n\n");
	printf("Usage:\n");
	printf("  gp_upgrade [OPTION]...\n");

	printf("\nGeneral options:\n");
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user (must be superuser!)\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  -v                       verbose output\n"));


}



int
main(int argc, char **argv)
{
    char conninfo[1024];
    PGconn     *conn;
    PGresult   *res;
    int         i;

    
    typedef struct 
    {
    	int content;
    	bool definedprimary ;
    	int dbid ;
    	bool isprimary ;
    	bool valid ;
    	char * hostname ;
    	char * port ;
    	PGconn     *conn;
    	PGresult   *res;
    } config_t;
           
           
    config_t * segdbs;  
    int segdbcount = 0;
    
    char ** databasenames;
    int	databasecount = 0;
    
    char * masterDir = NULL;
    
    static struct option long_options[] =
    {
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"username", required_argument, NULL, 'U'},
        {"version", no_argument, NULL, 'V'},
        {"password", no_argument, NULL, 'W'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    int         optindex;
    extern char *optarg;
    extern int  optind;
    int         c;
    int d;
    
    char * dbname = 0;
    char * host = 0;
    char * port = 0;
    char * username = 0;

    char * version = 0;
    bool isMPP;
    bool force_password = false;
    
    
    if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("gp_upgrade (Greenplum Database) " PG_VERSION);
			exit(0);
		}
	}


    while ((c = getopt_long(argc, argv, "h:p:U:vVW?",
                            long_options, &optindex)) != -1)
       {
        switch (c)
        {

            case 'h':
                host = optarg;
                break;
            case 'p':
                port = optarg;
                break;

            case 'U':
                username = optarg;
                break;
            case 'W':
	            force_password = true;
                break;
                
            case 'v':
				verbose = true;
				break;
				
             case '?':
                /* Actual help option given */
                if (strcmp(argv[optind - 1], "-?") == 0 || strcmp(argv[optind - 1], "--help") == 0)
                {
                    help();
                    exit(EXIT_SUCCESS);
                }
                
                /* unknown option reported by getopt */
                else
                {
                    
                    exit(EXIT_FAILURE);
                }
                break;
            default:

                exit(EXIT_FAILURE);
                break;
        }
    }
  
    /*
     * if we still have arguments, use it as the username
     */
    while (argc - optind >= 1)
    {
       if (!username)
            username = argv[optind];
        else 
            fprintf(stderr, "%s: warning: extra command-line argument \"%s\" ignored\n",
                    argv[0], argv[optind]);

        optind++;
    }
 

	dbname = "template1";  


    /*
     * If the user supplies a parameter on the command line, use it as the
     * conninfo string; otherwise default to setting dbname=postgres and using
     * environment variables or defaults for all other connection parameters.
     */
     
     conninfo[0] = '\0';
     if (host)
     {
     	strcat(conninfo,"host=");
     	strcat(conninfo,host);
     }
     if (port)
     {
     	strcat(conninfo," port=");
     	strcat(conninfo,port);
     }
     if (dbname)
     {
     	strcat(conninfo," dbname=");
     	strcat(conninfo,dbname);
     }
     if (username)
     {
     	strcat(conninfo," user=");
     	strcat(conninfo,username);
     }
     
     

     strcat(conninfo , " options='-O -c mpp_session_role=utility'");
     
     printf("gp_upgrade running.\n");

    /* Make a connection to the database */
    conn = connect_to_db(conninfo, force_password);
    
    res = PQexec(conn, "SELECT VERSION()");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "SELECT VERSION() failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        fprintf(stderr," UPGRADE FAILED\n");
        exit(1);
    }

    version = PQgetvalue(res, 0, 0);
    
    if (strchr(version,')') > 0)
    	*(strchr(version,')')+1) = '\0';


    printf("gp_upgrade:  Version of system:  %s\n", version);

    if (strstr(version,"Greenplum Database")!= 0 || strstr(version,"Greenplum database")!= 0 || strstr(version,"Bizgres MPP")!= 0)
        isMPP = 1;
    else
        isMPP = 0;
        
    if (!isMPP)
    {	
    	fprintf(stderr,"This is not a Greenplum Database system");
    	PQfinish(conn);
    	exit(1);
    }

	if (strncmp(version,"PostgreSQL 8.1.5",strlen("PostgreSQL 8.1.5"))!=0)
	{
		fprintf(stderr,"You must upgrade the Greenplum Database software to Greenplum Database 2.3, and restart the system, before running this utility\n");
		PQfinish(conn);
		exit(1);
		
	}
	
    PQclear(res);
    
    
    
    
    res = PQexec(conn, "select datname from pg_catalog.pg_database order by oid");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "select datname from pg_database: command failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        fprintf(stderr," UPGRADE FAILED\n");
    	exit(1);
    }
    
    databasenames = malloc(sizeof(char *) * PQntuples(res));
    databasecount = PQntuples(res);
    for (i = 0; i < databasecount; i++)
    {
    	databasenames[i] = strdup(PQgetvalue(res, i, 0));
    }
	PQclear(res);

    res = PQexec(conn, "select * from pg_catalog.gp_configuration where dbid = 1 or content >= 0 order by content, definedprimary desc, dbid");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "select * from gp_configuration: command failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
   		exit(1);
    }

	segdbs = malloc(sizeof(config_t) * PQntuples(res));
	segdbcount = PQntuples(res);


    /* next, save the rows */
    for (i = 0; i < segdbcount; i++)
    {
    	segdbs[i].content = atol(PQgetvalue(res, i, 0));
    	segdbs[i].definedprimary = PQgetvalue(res, i, 1)[0] == 't';
    	segdbs[i].dbid = atol(PQgetvalue(res, i, 2));
    	segdbs[i].isprimary = PQgetvalue(res, i, 3)[0] == 't';
    	segdbs[i].valid = PQgetvalue(res, i, 4)[0] == 't';
    	if (i == 0 && segdbs[i].content == -1 && host != NULL)
    		segdbs[i].hostname = host;
    	else 
    		segdbs[i].hostname = strdup(PQgetvalue(res, i, 5));
    	segdbs[i].port = strdup(PQgetvalue(res, i, 6));
    	if (i == 0 && segdbs[i].content == -1)
    		masterDir = strdup(PQgetvalue(res,i,7));
    }
    
    
    PQclear(res);
    
    segdbs[0].conn = conn;
    
    printf("gp_upgrade:  Got configuration information from entry db.\n");

    for (d = 0; d < databasecount; d++)
    {
    	
    	
    		
    	printf("gp_upgrade:  Upgrading database %s\n",databasenames[d]);
    	
    	for (i = 0; i < segdbcount; i++)
	    {
	    	char conninfo[1024];
	    	sprintf(conninfo,"host=%s port=%s dbname=%s options='-O -c mpp_session_role=utility' ",
	    		segdbs[i].hostname, segdbs[i].port, databasenames[d]);
	
		    /* Make a connection to the database */
		   	segdbs[i].conn = connect_to_db(conninfo,false);
		   
		   	if (d ==0)
		   	{
		   		if (verbose)
		    	printf("\nupdating segdb content %d definedprimary %d dbid %d\n\n",segdbs[i].content,segdbs[i].definedprimary,segdbs[i].dbid);
		   		
		   		run_sql_command(segdbs[i].conn, "update pg_database set datallowconn = 't' where datname='template0'");
		   	}
	    	
	    }
	    
	    /*
	     * We now have a connection open to the database on every segDB (0 is masterDB)
	     */
	    
	    for (i = 0; i < segdbcount; i++)
	    {
	    	
		    conn = segdbs[i].conn;
		    
		    if (verbose)
		    	printf("\nupdating segdb content %d definedprimary %d dbid %d for database %s\n\n",segdbs[i].content,segdbs[i].definedprimary,segdbs[i].dbid,databasenames[d]);
		    
		    run_sql_command(conn, pg_resqueue);
		    
		    run_sql_command(conn, pg_resqueue_index1);
		    
		    run_sql_command(conn, pg_resqueue_index2);
		    
		    run_sql_command(conn, pg_authid);
		    
		    if (d==0)
		  	{
		  		run_sql_command(conn, "VACUUM FULL pg_authid");
		  	}
		    
		    run_sql_command(conn, pg_authid_index1);
		    
		    run_sql_command(conn, "DROP VIEW pg_catalog.pg_roles CASCADE");
		    run_sql_command(conn, pg_roles);
		   	    
		   	run_sql_command(conn, "DROP VIEW pg_catalog.pg_locks CASCADE");
		  	run_sql_command(conn, pg_locks);
		  	
		  	
		  	run_sql_command(conn, "UPDATE pg_catalog.pg_proc SET probin='$libdir/plpgsql' WHERE proname IN ('plpgsql_call_handler', 'plpgsql_validator') AND substr(probin,1,1)='/' ");
		  	
		    
		    if (strcmp(databasenames[d],"template1")==0 ||
		    	strcmp(databasenames[d],"template0")==0)
		    	{
		    		run_sql_command(conn, "VACUUM FULL");
		    		run_sql_command(conn, "VACUUM FREEZE");
		    	}
	    }
    	
    }

	
	for (i = 0; i < segdbcount; i++)
    { 	
    	if (verbose)
		    	printf("\nupdating segdb content %d definedprimary %d dbid %d\n\n",segdbs[i].content,segdbs[i].definedprimary,segdbs[i].dbid);
	
	    conn = segdbs[i].conn;
		run_sql_command(conn, "update pg_database set datallowconn = 'f' where datname='template0'");
    }
		 
		 
	if (masterDir)
	{
		FILE * fp;
		char conffilename[1024];
		char * conf;
		sprintf(conffilename, "%s/postgresql.conf",masterDir);
		fp = fopen(conffilename,"r+");
		if (fp == NULL)
			fprintf(stderr,"Can't add comments to postgresql.conf, can't open the file\n");
		else
		{
			size_t s;
			conf = malloc(64000);
			s = fread(conf,1,63999,fp);
			conf[s] = '\0';
			
			if (strstr(conf,"resource_scheduler")!=0)
				printf("postgresql.conf already upgraded, skipping that step.\n");
			else
			{
			
				strcat(conf,newconfinfo);
				
				fseek(fp,0,SEEK_SET);
				
				fwrite(conf,1,strlen(conf),fp);
			}
			fclose(fp);
			
			
		}
		
	}
	
   

	printf("Done\n");
    /* close the connection to the database and cleanup */
    PQfinish(conn);


	
	return(0);	
}
