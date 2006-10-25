#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "libpq-fe.h"

typedef int bool;

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




/* NOTE: These have tab characters in the strings... Be sure you don't lose them when editing this file  
static char * pg_class_data = 
"6027	pg_resqueue_oid_index	11	0	10	403	6027	1664	1	0	0	0	f	t	i	1	0	0	0	0	0	f	f	f	f	\\N\n"
"6028	pg_resqueue_rsqname_index	11	0	10	403	6028	1664	1	0	0	0	f	t	i	1	0	0	0	0	0	f	f	f	f	\\N\n"
"6026	pg_resqueue	11	10288	10	0	6026	1664	0	0	0	0	t	t	r	3	0	0	0	0	0	t	f	f	f	\\N\n"
"6029	pg_authid_rolresqueue_index	11	0	10	403	6029	1664	2	1	0	0	f	t	i	1	0	0	0	0	0	f	f	f	f	\\N\n";

static char * pg_type_data =
"10284	pg_resqueue	11	10	-1	f	c	t	,	6026	0	record_in	record_out	record_recv	record_send	-	d	x	f	0	-1	0	\\N	\\N\n";

static char * pg_index_data =
"6027	6026	1	t	f	f	-2	1989	\\N	\\N\n"
"6028	6026	1	t	f	f	1	1986	\\N	\\N\n"
"6029	1260	1	f	f	f	12	1989	\\N	\\N\n";

static char * pg_attribute_data =
"6026	rsqname	19	-1	64	1	0	-1	-1	f	p	i	t	f	f	t	0\n"
"6026	rsqcountlimit	700	-1	4	2	0	-1	-1	f	p	i	t	f	f	t	0\n"
"6026	rsqcostlimit	700	-1	4	3	0	-1	-1	f	p	i	t	f	f	t	0\n"
"6026	ctid	27	0	6	-1	0	-1	-1	f	p	s	t	f	f	t	0\n"
"6026	oid	26	0	4	-2	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6026	xmin	28	0	4	-3	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6026	cmin	29	0	4	-4	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6026	xmax	28	0	4	-5	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6026	cmax	29	0	4	-6	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6026	tableoid	26	0	4	-7	0	-1	-1	t	p	i	t	f	f	t	0\n"
"6027	oid	26	-1	4	1	0	-1	-1	t	p	i	f	f	f	t	0\n"
"6028	rsqname	19	-1	64	1	0	-1	-1	f	p	i	f	f	f	t	0\n"
"6029	rolresqueue	26	-1	4	1	0	-1	-1	t	p	i	f	f	f	t	0\n";

*/

static char * pg_locks = 
   "CREATE OR REPLACE VIEW pg_catalog.pg_locks AS "
   "SELECT * "
   "FROM pg_lock_status() AS L "
   "(locktype text, database oid, relation oid, page int4, tuple int2,"
   "  transactionid xid, classid oid, objid oid, objsubid int2,"
    " transaction xid, pid int4, mode text, granted boolean, mppSessionId int4, mppIsWriter boolean)";
     


static char * pg_roles = 
	"CREATE OR REPLACE VIEW pg_catalog.pg_roles AS "
	"SELECT pg_authid.rolname, pg_authid.rolsuper, pg_authid.rolinherit, pg_authid.rolcreaterole, pg_authid.rolcreatedb, pg_authid.rolcatupdate, pg_authid.rolcanlogin, pg_authid.rolconnlimit, '********'::text AS rolpassword, pg_authid.rolvaliduntil, pg_authid.rolconfig, pg_authid.rolresqueue, pg_authid.oid FROM pg_authid";
   
 
static void MyNoticeProcessor (void *arg, const char *message)
{
	fprintf(stderr,"%s",message);
} 


static PGconn     *
connect_to_db(char * conninfo)
{
	PGconn     *conn;
    PGresult   *res;
	
    /* Make a connection to the database */
    conn = PQconnectdb(conninfo);

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
	
	res = PQexec(conn, cmd);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
    	 SqlState[0] = '\0';
         if (PQresultErrorField(res, PG_DIAG_SQLSTATE) != NULL)
            strcpy(SqlState, PQresultErrorField(res, PG_DIAG_SQLSTATE));
            
    	if (strcmp(SqlState,"42P07")!=0 &&  /* Table already exists */
    	    strcmp(SqlState,"42701")!=0)	/* Column already exists */
    	{
	        fprintf(stderr, " %s: command failed: %s %s", cmd,SqlState, PQerrorMessage(conn));
	        //PQclear(res);
	        //PQfinish(conn);
	    	//exit(1);
    	}
    	PQclear(res);
    }
    else
    	PQclear(res);
	
}


/*
static void 
do_copy(PGconn * conn, char * copycommand, char * data)
{
	PGresult   *res;
	res = PQexec(conn, copycommand);

    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        fprintf(stderr, "COPY failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        exit(1);
    }
    PQclear(res);
    
    if (PQputCopyData(conn, data, strlen(data)) != 1)
    {
        fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        exit(1);
    }
    
     if (PQputCopyEnd(conn,0) == -1)
    {
        fprintf(stderr, "putCOPYEnd failed: %s", PQerrorMessage(conn));
        PQfinish(conn);
        exit(1);
    }
    res = PQgetResult(conn);

    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "copy failed: %s", PQerrorMessage(conn));
       
        
    }
    PQclear(res);
}
*/

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
    
    static struct option long_options[] =
    {
        {"dbname", required_argument, NULL, 'd'},
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"username", required_argument, NULL, 'U'},
        {"set", required_argument, NULL, 'v'},
        {"variable", required_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"password", no_argument, NULL, 'W'},
        {"expanded", no_argument, NULL, 'x'},
        {"no-psqlrc", no_argument, NULL, 'X'},
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
    char * password = 0;
    char * version = 0;
    bool isMPP;


    while ((c = getopt_long(argc, argv, "d:h:p:U:v:VWxX?",
                            long_options, &optindex)) != -1)
       {
        switch (c)
        {

            case 'd':
                dbname = optarg;
                break;
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
                password = optarg;
                break;
             case '?':
                /* Actual help option given */
                if (strcmp(argv[optind - 1], "-?") == 0 || strcmp(argv[optind - 1], "--help") == 0)
                {
                    //usage();
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
     * if we still have arguments, use it as the database name and username
     */
    while (argc - optind >= 1)
    {
        if (!dbname)
            dbname = argv[optind];
        else if (!username)
            username = argv[optind];
        else 
            fprintf(stderr, "%s: warning: extra command-line argument \"%s\" ignored\n",
                    argv[0], argv[optind]);

        optind++;
    }
 
	if (!dbname)
		dbname = "template1";  // Maybe should use postgres database?


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
      if (password)
     {
     	strcat(conninfo," password=");
     	strcat(conninfo,password);
     }
     

     strcat(conninfo , " options='-O -c mpp_session_role=utility'");
     
     printf("mpp_upgrade running.\n");

    /* Make a connection to the database */
    conn = connect_to_db(conninfo);
    
    res = PQexec(conn, "SELECT VERSION()");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "SELECT VERSION() failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
        exit(1);
    }

    version = PQgetvalue(res, 0, 0);


    printf("mpp_upgrade:  Version of system:  %s\n", version);

    if (strstr(version,"Bizgres MPP")!= 0)
        isMPP = 1;
    else
        isMPP = 0;

    PQclear(res);
    
    if (!isMPP)
    {	
    	fprintf(stderr,"This is not a Bizgres MPP system");
    	PQfinish(conn);
    	exit(1);
    }
    
    
    res = PQexec(conn, "select datname from pg_catalog.pg_database order by oid");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "select datname from pg_database: command failed: %s", PQerrorMessage(conn));
        PQclear(res);
        PQfinish(conn);
    	exit(1);
    }
    
    databasenames = malloc(sizeof(char *) * PQntuples(res));
    databasecount = PQntuples(res);
    for (i = 0; i < databasecount; i++)
    {
    	databasenames[i] = strdup(PQgetvalue(res, i, 0));
    	printf("%s\n",databasenames[i]);
    }
	PQclear(res);

    res = PQexec(conn, "select * from pg_catalog.mpp_configuration where dbid = 1 or content >= 0 order by content, definedprimary desc, dbid");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "select * from mpp_configuration: command failed: %s", PQerrorMessage(conn));
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
    	segdbs[i].hostname = strdup(PQgetvalue(res, i, 5));
    	segdbs[i].port = strdup(PQgetvalue(res, i, 6));
    }
    
    
    PQclear(res);
    
    segdbs[0].conn = conn;
    
    printf("mpp_upgrade:  Got configuration information from entry db.\n");

    for (d = 0; d < databasecount; d++)
    {
    	
    	
    		
    	printf("mpp_upgrade:  Upgrading database %s\n",databasenames[d]);
    	
    	for (i = 0; i < segdbcount; i++)
	    {
	    	char conninfo[1024];
	    	sprintf(conninfo,"host=%s port=%s dbname=%s options='-O -c mpp_session_role=utility' ",
	    		segdbs[i].hostname, segdbs[i].port, databasenames[d]);
	
		    /* Make a connection to the database */
		   	segdbs[i].conn = connect_to_db(conninfo);
		   
		   	if (d ==0)
		   		run_sql_command(segdbs[i].conn, "update pg_database set datallowconn = 't' where datname='template0'");
	    	
	    }
	    
	    /*
	     * We now have a connection open to template1 on every segDB (0 is masterDB)
	     */
	    
	    for (i = 0; i < segdbcount; i++)
	    {
	    	
		    conn = segdbs[i].conn;
		    
		    run_sql_command(conn, pg_resqueue);
		    
		    run_sql_command(conn, pg_resqueue_index1);
		    
		    run_sql_command(conn, pg_resqueue_index2);
		    
		    run_sql_command(conn, pg_authid);
		    
		    if (d==0)
		  	{
		  		run_sql_command(conn, "vacuum full pg_authid");
		  	}
		    
		    run_sql_command(conn, pg_authid_index1);
		    
		    run_sql_command(conn, "drop view pg_catalog.pg_roles cascade");
		    run_sql_command(conn, pg_roles);
		   	    
		   	run_sql_command(conn, "drop view pg_catalog.pg_locks cascade");
		  	run_sql_command(conn, pg_locks);
		  	
		  	
		    
		    if (strcmp(databasenames[d],"template1")==0 ||
		    	strcmp(databasenames[d],"template0")==0)
		    	{
		    		run_sql_command(conn, "vacuum full");
		    		run_sql_command(conn, "vacuum freeze");
		    	}
	    }
    	
    }
	/*
	do_copy(conn, "COPY pg_catalog.pg_class FROM STDIN WITH OIDS ", pg_class_data);
	do_copy(conn, "COPY pg_catalog.pg_type FROM STDIN WITH OIDS ", pg_type_data);
	do_copy(conn, "COPY pfullg_catalog.pg_index FROM STDIN ", pg_index_data);
	do_copy(conn, "COPY pg_catalog.pg_attribute FROM STDIN ", pg_attribute_data);
	*/

	
	for (i = 0; i < segdbcount; i++)
    { 	
	    conn = segdbs[i].conn;
		run_sql_command(conn, "update pg_database set datallowconn = 'f' where datname='template0'");
    }
		 
	
   

	printf("Done\n");
    /* close the connection to the database and cleanup */
    PQfinish(conn);


	
	return(0);	
}
