/*
* $Id$
*
* Revision History
* ===================
* $Log: load_stub.c,v $
* Revision 1.10  2007/09/12 20:49:18  cmcdevitt
* fix 64-bit portability issue
*
* Revision 1.9  2007/04/10 00:08:54  cmcdevitt
* Load directly into partitioned orders and lineitem with -p option
*
* Revision 1.8  2007/04/07 18:51:35  cmcdevitt
* support 2.3 and 3.0 GPDB
*
* Revision 1.7  2007/04/07 08:10:40  cmcdevitt
* Fixes for dbgen with large scale factors
*
* Revision 1.6  2007/03/21 23:29:30  cmcdevitt
* name change
*
* Revision 1.5  2006/11/03 23:30:25  cmcdevitt
* Bizgres MPP is now Greenplum database
*
* Revision 1.4  2006/11/03 22:42:46  cmcdevitt
* Bizgres MPP is now Greenplum database
*
* Revision 1.3  2006/05/26 18:38:52  tkordas
* Fixup to work slightly better on the Mac.
*
* Revision 1.2  2006/04/27 00:21:49  cmcdevitt
* Fix for 64-bit machines
*
* Revision 1.1  2006/04/18 23:17:18  cmcdevitt
* Add TPC-H code.. dbgen, qgen, and tpchdriver
*
* Revision 1.2  2005/01/03 20:08:58  jms
* change line terminations
*
* Revision 1.1.1.1  2004/11/24 23:31:46  jms
* re-establish external server
*
* Revision 1.1.1.1  2003/04/03 18:54:21  jms
* recreation after CVS crash
*
* Revision 1.1.1.1  2003/04/03 18:54:21  jms
* initial checkin
*
*
*/
/*****************************************************************
*  Title:      load_stub.c
*  Description:
*              stub routines for:
*          inline load of dss benchmark
*          header creation for dss benchmark
*
*****************************************************************
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#define snprintf _snprintf
#endif 
#ifdef BINARY
#ifdef WIN32
#include <winsock2.h>
#define snprintf _snprintf
#else
#include <netinet/in.h>
#endif 
#endif
#include <assert.h>
#include "libpq-fe.h"

#include "config.h"
#include "dss.h"
#include "dsstypes.h"

#include "cdbhash.h"

static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";
static int flags = 0;
static int hext = 0;


static int isMPP = 0;

static CdbHash * h = 0;

int isPartitioned = 0;


static char * base_orders_name = "ORDERS";
static char * base_lineitem_name = "LINEITEM";

static int numparts = 16;

static char * partnames[] = { "",
							"_child_depth_1_y_1992_year",
							"_child_depth_1_y_1993_year",
							"_child_depth_1_y_1994_year",
							"_child_depth_1_y_1995_year",
							"_child_depth_1_y_1996_year",
							"_child_depth_1_y_1997_year",
							"_child_depth_2_ym_1998_01_month",
							"_child_depth_2_ym_1998_02_month",
							"_child_depth_2_ym_1998_03_month",
							"_child_depth_2_ym_1998_04_month",
							"_child_depth_2_ym_1998_05_month",
							"_child_depth_2_ym_1998_06_month",
							"_child_depth_2_ym_1998_07_month",
							"_child_depth_2_ym_1998_08_month",
							"_child_depth_1_y_1998_year",
							0};
							

	
static char * part_start[] = { "", 
								"1992-01-01",
								"1993-01-01",			
								"1994-01-01",	
								"1995-01-01",	
								"1996-01-01",	
								"1997-01-01",	
								"1998-01-01",	
								"1998-02-01",
								"1998-03-01",
								"1998-04-01",
								"1998-05-01",
								"1998-06-01",
								"1998-07-01",
								"1998-08-01",
								"1998-09-01",
								"1999-01-01",
								0 }; /* must be one longer than partnames */
					
		
								
								
								

PGconn * connectToDb()
{
	PGconn * conn;
	PGresult   *res = 0;
	char schema[250];
	char conninfo[250];
	strcpy(conninfo, db_name);

	if (numSegs > 0)
		strcat(conninfo , " options='-c gp_session_role=utility'");

	conn = PQconnectdb(conninfo);

	if (numSegs >0 && PQstatus(conn) != CONNECTION_OK && strstr(PQerrorMessage(conn),"unrecognized configuration parameter") != NULL)
	{
		strcpy(conninfo, db_name);

		if (numSegs > 0)
			strcat(conninfo , " options='-c mpp_session_role=utility'");

		conn = PQconnectdb(conninfo);
	}

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
			PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}


	if (schema_name && strlen(schema_name)>0)
	{



		strcpy(schema,"set search_path=");
		strcat(schema,schema_name);
		/*strcat(schema,",public,pg_catalog");*/
		res = PQexec(conn, schema);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "SET SEARCH_PATH failed: %s", PQerrorMessage(conn));
			PQclear(res);
			PQfinish(conn);
			exit(1);
		}
		PQclear(res);
	}

	return conn;
}

int 
prep_direct(void)
{
	PGconn * conn = 0;
	PGresult   *res = 0;
	const char * version = 0;

	/* any preload prep goes here */

	if (db_name == NULL)
		db_name = "";

	printf("Loading TPC-H data to %s\n", db_name);
	
	if (isPartitioned)
		numparts = 16;
	else
		numparts = 1;
		
	printf("Partitions for lineitem and orders = %d\n",numparts);

	if (step > 0 || numSegs > 0)
		return(0);

	/* Make a connection to the database */
	conn = PQconnectdb(db_name);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(conn) != CONNECTION_OK)
	{
		fprintf(stderr, "Connection to database failed: %s",
			PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	res = PQexec(conn, "SELECT VERSION()");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		fprintf(stderr, "SELECT VERSION() failed: %s", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}

	version = PQgetvalue(res, 0, 0);


	printf("Version of system:  %s\n", version);

	if (strstr(version,"Bizgres MPP")!= 0 || strstr(version,"Greenplum database")!= 0 || strstr(version,"Greenplum Database")!= 0)
		isMPP = 1;
	else
		isMPP = 0;

	PQclear(res);

	if (numSegs == 0)
	{

		printf("Creating the TPC-H schema and tables\n");

		if (schema_name && strlen(schema_name)>0)
		{
			char schema[250];
			strcpy(schema,"CREATE SCHEMA ");
			strcat(schema,schema_name);
			res = PQexec(conn, schema);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				if (strstr(PQerrorMessage(conn),"already exists")==0)
				{
					fprintf(stderr, "CREATE SCHEMA failed: %s", PQerrorMessage(conn));
					PQclear(res);
					PQfinish(conn);
					exit(1);
				}
				else
					if (verbose>0)
						printf("Schema %s already exists\n",schema_name);
			}
			else
				if (verbose>0)
					printf("Schema %s created\n",schema_name);
			PQclear(res);

			strcpy(schema,"set search_path=");
			strcat(schema,schema_name);
			/*strcat(schema,",public,pg_catalog");*/
			res = PQexec(conn, schema);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "SET SEARCH_PATH failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}


		if ((table & (1 << CUST)) != 0)
		{
			char createcustomer[512];

			snprintf(createcustomer,511,"CREATE TABLE CUSTOMER ( C_CUSTKEY     %s NOT NULL,"
				"     C_NAME        VARCHAR(25) NOT NULL,"
				"     C_ADDRESS     VARCHAR(40) NOT NULL,"
				"     C_NATIONKEY   INTEGER NOT NULL,"
				"     C_PHONE       CHAR(15) NOT NULL,"
				"     C_ACCTBAL     DECIMAL(15,2)   NOT NULL,"
				"     C_MKTSEGMENT  CHAR(10) NOT NULL,"
				"     C_COMMENT     VARCHAR(117) NOT NULL)", scale>300?"INT8":"INTEGER");


			if (force)
			{
				res = PQexec(conn, "DROP TABLE CUSTOMER");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating CUSTOMER table\n");

			res = PQexec(conn, createcustomer);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}


		if ((table & (1 << PART)) != 0 || (table & (1 << PART_PSUPP)) != 0)
		{

			char createpart[512];

			snprintf(createpart,511,"CREATE TABLE PART  ( P_PARTKEY     %s NOT NULL,"
				"  P_NAME        VARCHAR(55) NOT NULL,"
				"  P_MFGR        CHAR(25) NOT NULL,"
				"  P_BRAND       CHAR(10) NOT NULL,"
				"  P_TYPE        VARCHAR(25) NOT NULL,"
				"  P_SIZE        INTEGER NOT NULL,"
				"  P_CONTAINER   CHAR(10) NOT NULL,"
				"  P_RETAILPRICE DECIMAL(15,2) NOT NULL,"
				"  P_COMMENT     VARCHAR(23) NOT NULL )", scale>300?"INT8":"INTEGER");

			if (force)
			{
				res = PQexec(conn, "DROP TABLE PART");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating PART table\n");

			res = PQexec(conn, createpart);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}


		if ((table & (1 << PSUPP)) != 0 || (table & (1 << PART_PSUPP)) != 0)
		{
			char createpartsupp[512];

			snprintf(createpartsupp,511,"CREATE TABLE PARTSUPP ( PS_PARTKEY     %s NOT NULL,"
				"   PS_SUPPKEY     INTEGER NOT NULL,"
				"   PS_AVAILQTY    INTEGER NOT NULL,"
				"   PS_SUPPLYCOST  DECIMAL(15,2)  NOT NULL,"
				"   PS_COMMENT     VARCHAR(199) NOT NULL )", scale>300?"INT8":"INTEGER");

			if (force)
			{
				res = PQexec(conn, "DROP TABLE PARTSUPP");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating PARTSUPP table\n");

			res = PQexec(conn, createpartsupp);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}


		if ((table & (1 << SUPP)) != 0)
		{


			if (force)
			{
				res = PQexec(conn, "DROP TABLE SUPPLIER");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating SUPPLIER table\n");

			res = PQexec(conn, "CREATE TABLE SUPPLIER ( S_SUPPKEY     INTEGER NOT NULL,"
				" S_NAME        CHAR(25) NOT NULL,"
				" S_ADDRESS     VARCHAR(40) NOT NULL,"
				" S_NATIONKEY   INTEGER NOT NULL,"
				" S_PHONE       CHAR(15) NOT NULL,"
				" S_ACCTBAL     DECIMAL(15,2) NOT NULL,"
				" S_COMMENT     VARCHAR(101) NOT NULL)");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}

		if ((table & (1 << ORDER)) != 0 || (table & (1 << ORDER_LINE)) != 0)
		{
			char createorders[512];
			char orders_tablename[64];
			int i;
			
			for (i=0;i<numparts;i++)
			{
			 
			strcpy(orders_tablename,base_orders_name);
			strcat(orders_tablename,partnames[i]);
			
			if (i==0)

			snprintf(createorders,511,"CREATE TABLE %s  ( O_ORDERKEY       %s NOT NULL,"
				"  O_CUSTKEY        %s NOT NULL,"
				"  O_ORDERSTATUS    CHAR(1) NOT NULL,"
				"  O_TOTALPRICE     DECIMAL(15,2) NOT NULL,"
				"  O_ORDERDATE      DATE NOT NULL,"
				"  O_ORDERPRIORITY  CHAR(15) NOT NULL,"  
				"  O_CLERK          CHAR(15) NOT NULL,"
				"  O_SHIPPRIORITY   INTEGER NOT NULL,"
				"  O_COMMENT        VARCHAR(79) NOT NULL)", base_orders_name, scale>300?"INT8":"INTEGER", scale>300?"INT8":"INTEGER");
			else
				snprintf(createorders,511,"CREATE TABLE %s () INHERITS(%s)",orders_tablename,base_orders_name);
				
			if (force)
			{
				char droporders[512];
				sprintf(droporders, "DROP TABLE %s CASCADE",orders_tablename);
				res = PQexec(conn, droporders);
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating %s table\n",orders_tablename);

			res = PQexec(conn, createorders);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
			}

		}

		if ((table & (1 << LINE)) != 0 || (table & (1 << ORDER_LINE)) != 0)
		{

			char createlineitem[1024];
			char lineitem_tablename[64];
			int i;
			
			for (i=0;i<numparts;i++)
			{
			 
			strcpy(lineitem_tablename,base_lineitem_name);
			strcat(lineitem_tablename,partnames[i]);
			
			if (i==0)
			snprintf(createlineitem,1023,"CREATE TABLE %s ( L_ORDERKEY    %s NOT NULL,"
				" L_PARTKEY     %s NOT NULL,"
				" L_SUPPKEY     INTEGER NOT NULL,"
				" L_LINENUMBER  INTEGER NOT NULL,"
				" L_QUANTITY    DECIMAL(15,2) NOT NULL,"
				" L_EXTENDEDPRICE  DECIMAL(15,2) NOT NULL,"
				" L_DISCOUNT    DECIMAL(15,2) NOT NULL,"
				" L_TAX         DECIMAL(15,2) NOT NULL,"
				" L_RETURNFLAG  CHAR(1) NOT NULL,"
				" L_LINESTATUS  CHAR(1) NOT NULL,"
				" L_SHIPDATE    DATE NOT NULL,"
				" L_COMMITDATE  DATE NOT NULL,"
				" L_RECEIPTDATE DATE NOT NULL,"
				" L_SHIPINSTRUCT CHAR(25) NOT NULL,"
				" L_SHIPMODE     CHAR(10) NOT NULL,"
				" L_COMMENT      VARCHAR(44) NOT NULL)",base_lineitem_name, scale>300?"INT8":"INTEGER", scale>300?"INT8":"INTEGER");
			else
				snprintf(createlineitem,511,"CREATE TABLE %s () INHERITS(%s)",lineitem_tablename,base_lineitem_name);
			
			if (force)
			{
				char droplineitem[512];
				sprintf(droplineitem, "DROP TABLE %s CASCADE",lineitem_tablename);
				res = PQexec(conn, droplineitem);
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating %s table\n",lineitem_tablename);

			res = PQexec(conn, createlineitem);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
			}
		}

		if ((table & (1 << NATION)) != 0)
		{

			if (force)
			{
				res = PQexec(conn, "DROP TABLE NATION");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating NATION table\n");

			res = PQexec(conn, "CREATE TABLE NATION  ( N_NATIONKEY  INTEGER NOT NULL,"
				" N_NAME       CHAR(25) NOT NULL,"
				" N_REGIONKEY  INTEGER NOT NULL,"
				" N_COMMENT    VARCHAR(152))");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);

		}

		if ((table & (1 << REGION)) != 0)
		{
			if (force)
			{
				res = PQexec(conn, "DROP TABLE REGION");
				PQresultStatus(res);
				PQclear(res);
			}

			if (verbose > 0)
				printf("Creating REGION table\n");

			res = PQexec(conn, "CREATE TABLE REGION  ( R_REGIONKEY  INTEGER NOT NULL,"
				"  R_NAME       CHAR(25) NOT NULL,"
				"  R_COMMENT    VARCHAR(152))");
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				fprintf(stderr, "CREATE TABLE failed: %s", PQerrorMessage(conn));
				PQclear(res);
				PQfinish(conn);
				exit(1);
			}
			PQclear(res);
		}

		if (verbose >0)
			printf("TPC-H schema and tables created\n");
	}

	PQfinish(conn);

	return(0);
}


int 
close_direct(void)
{
	PGresult   *res = 0;
	PGconn * conn = 0;

	if (step>0 || numSegs > 0)
		return(0);

	if (numSegs == 0)
	{
		
		conn = connectToDb();

		printf("Creating indexes on the tables\n");

		if (verbose >0)
			printf("ALTER TABLE ORDERS ADD CONSTRAINT orders_pkey PRIMARY KEY (O_ORDERKEY)\n");
		res = PQexec(conn, "ALTER TABLE ORDERS ADD CONSTRAINT orders_pkey PRIMARY KEY (O_ORDERKEY)");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ALTER TABLE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("CREATE INDEX L_ORDERKEY_IX\n");
		res = PQexec(conn, "CREATE INDEX L_ORDERKEY_IX ON lineitem (l_orderkey)");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "CREATE INDEX failed: %s", PQerrorMessage(conn));
		PQclear(res);

		printf("Running ANALYZE on the tables\n");



		if (verbose >0)
			printf("ANALYZE REGION\n");
		res = PQexec(conn, "ANALYZE REGION");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE NATION\n");
		res = PQexec(conn, "ANALYZE NATION");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE CUSTOMER\n");
		res = PQexec(conn, "ANALYZE CUSTOMER");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE PART\n");
		res = PQexec(conn, "ANALYZE PART");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE PARTSUPP\n");
		res = PQexec(conn, "ANALYZE PARTSUPP");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE SUPPLIER\n");
		res = PQexec(conn, "ANALYZE SUPPLIER");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE ORDERS\n");
		res = PQexec(conn, "ANALYZE ORDERS");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);

		if (verbose >0)
			printf("ANALYZE LINEITEM\n");
		res = PQexec(conn, "ANALYZE LINEITEM");
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			fprintf(stderr, "ANALYZE failed: %s", PQerrorMessage(conn));
		PQclear(res);


		PQfinish(conn);
		printf("ANALYZE finished... Tables are ready to use.\n");
	}





	return(0);
}


int 
finish_copy(PGconn * conn)
{
	PGresult   *res = 0;

	if (conn==NULL)
		return(0);
#ifdef BINARY

	if (PQputCopyData(conn,
		"\377\377", 2) != 1)  /* eof */
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}
#endif

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
		PQclear(res);
		return(1);
	}

	PQfinish(conn);

	return(0);
}


PGconn	   *cust_conn = 0;
PGconn	   *part_conn = 0;
PGconn	   *psupp_conn = 0;
PGconn	   *supp_conn = 0;
PGconn	   *order_conn = 0;
PGconn	   *line_conn = 0;
PGconn	   *nation_conn = 0;
PGconn	   *region_conn = 0;

PGconn 	   *line_part_conn[16] = { 0 };
PGconn	   *order_part_conn[16] = { 0 };


int
finish_one_load(int tnum)
{
	if (tnum==CUST)
	{
		finish_copy(cust_conn);
		cust_conn=0;
	}
	if (tnum==PART || tnum==PART_PSUPP)
	{
		finish_copy(part_conn);
		part_conn=0;
	}
	if (tnum==PSUPP || tnum==PART_PSUPP)
	{
		finish_copy(psupp_conn);
		psupp_conn=0;
	}
	if (tnum==SUPP)
	{
		finish_copy(supp_conn);
		supp_conn=0;
	}
	if (tnum==ORDER || tnum==ORDER_LINE)
	{
		if (isPartitioned)
		{
			int i;
			for (i=0;i<numparts;i++)
				if (order_part_conn[i] != 0)
					finish_copy(order_part_conn[i]);
		}
		else
		finish_copy(order_conn);
		order_conn=0;
	}
	if (tnum==LINE || tnum==ORDER_LINE)
	{
		if (isPartitioned)
		{
			int i;
			for (i=0;i<numparts;i++)
				if (line_part_conn[i] != 0)
					finish_copy(line_part_conn[i]);
		}
		else
		finish_copy(line_conn);
		line_conn=0;
	}
	if (tnum==NATION)
	{
		finish_copy(nation_conn);
		nation_conn=0;
	}
	if (tnum==REGION)
	{
		finish_copy(region_conn);
		region_conn=0;
	}


	return(0);
}

int 
hd_cust (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the customer table\n");

	return(0);
}

#define NUMERIC_SIGN_MASK	0xC000
#define NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_NAN			0xC000
#define NUMERIC_DSCALE_MASK 0x3FFF
#define NBASE 10000

int 
ld_cust (customer_t *cp, int mode)
{
	static int count = 0;
	PGresult   *res = 0;
	static short custcolumns = 8;
	static char buffer[512];
	static char * b;
	int dollars,
		cents;


	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		cust_conn = connectToDb();

		res = PQexec(cust_conn, "COPY CUSTOMER FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);

		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(cust_conn));
			PQclear(res);
			PQfinish(cust_conn);
			exit(1);
		}
		PQclear(res);

#ifdef BINARY
		if (PQputCopyData(cust_conn,
			(const char *)BinarySignature,
			sizeof(BinarySignature)) != 1)
		{
			fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(cust_conn));
			PQfinish(cust_conn);
			exit(1);
		}

		if (PQputCopyData(cust_conn,
			(const char *)&flags,
			sizeof(flags)) != 1)
		{
			fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(cust_conn));
			PQfinish(cust_conn);
			exit(1);
		}
		if (PQputCopyData(cust_conn,
			(const char *)&hext,
			sizeof(hext)) != 1)
		{
			fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(cust_conn));
			PQfinish(cust_conn);
			exit(1);
		}
#endif

	}


	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(cp->custkey)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}

#ifdef BINARY


	b = buffer;
	*(short *)b = htons(custcolumns);
	b += 2;

	*(int *)b = htonl(sizeof(int));
	b += sizeof(int);
	*(int *)b = htonl((long)cp->custkey);
	b += sizeof(int);

	*(int *)b = htonl(strlen(cp->name));
	b += sizeof(int);
	strcpy(b,cp->name);
	b += strlen(cp->name);

	*(int *)b = htonl(strlen(cp->address));
	b += sizeof(int);
	strcpy(b,cp->address);
	b += strlen(cp->address);

	*(int *)b = htonl(sizeof(int));
	b += sizeof(int);
	*(int *)b = htonl((long)cp->nation_code);
	b += sizeof(int);

	*(int *)b = htonl(strlen(cp->phone));
	b += sizeof(int);
	strcpy(b,cp->phone);
	b += strlen(cp->phone);

	{
		unsigned short len = 0;
		short weight = 0;
		unsigned short sign = cp->acctbal>=0? NUMERIC_POS : NUMERIC_NEG;
		unsigned short scale = 2;
		unsigned short parts[5] = { 0 };
		int i;

		DSS_HUGE temp = cp->acctbal>=0? cp->acctbal : -cp->acctbal;
		if (temp==0)
		{
			weight = 0;
			len = 0;
		}
		else
		{


			int temp2 = (int)(temp %100);
			temp = temp / 100;
			if (temp2)
				len++;
			while(temp>0)
			{
				parts[len++] = (unsigned short)(temp % NBASE);

				temp /= NBASE;
			}
			if (temp2)
			{
				parts[0] = (unsigned short)(temp2*100 % NBASE);
			}
		}


		*(int *)b = htonl(len*sizeof(short) + 8);
		b += sizeof(int);


		*(short *)b = htons(len);
		b += 2;  
		*(short *)b = htons(weight);
		b += 2;  
		*(short *)b = htons(sign);
		b += 2;  
		*(short *)b = htons(scale);
		b += 2;  


		for (i=0;i<len;i++)
		{
			*(short *)b = htons(parts[len-i-1]);
			b += 2;  
		}

	}

	*(int *)b = htonl(strlen(cp->mktsegment));
	b += sizeof(int);
	strcpy(b,cp->mktsegment);
	b += strlen(cp->mktsegment);

	*(int *)b = htonl(strlen(cp->comment));
	b += sizeof(int);
	strcpy(b,cp->comment);
	b += strlen(cp->comment);

#else



	dollars = (int)(cp->acctbal / 100);
	if (cp->acctbal < 0)
		cents = (int) ((- cp->acctbal) % 100);
	else
		cents = (int)(cp->acctbal % 100);

	assert(cp->custkey >= 0);

	b = buffer + sprintf(buffer, HUGE_FORMAT "|%s|%s|" HUGE_FORMAT "|%s|%d.%02d|%s|%s\n", 
		cp->custkey,cp->name,cp->address,cp->nation_code,cp->phone,dollars,cents,cp->mktsegment,cp->comment);



#endif 

	if (PQputCopyData(cust_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(cust_conn));
		PQfinish(cust_conn);
		exit(1);
	}


	return(0);
}

int 
hd_part (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the part table\n");

	return(0);
}

int 
ld_part (part_t *pp, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[512];
	static char * b;
	int dollars,
		cents;



	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		part_conn = connectToDb();




		res = PQexec(part_conn, "COPY PART FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(part_conn));
			PQclear(res);
			PQfinish(part_conn);
			exit(1);
		}
		PQclear(res);



	}



	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(pp->partkey)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}



	dollars = (int)(pp->retailprice / 100);
	if (pp->retailprice < 0)
		cents = (int) ((- pp->retailprice) % 100);
	else
		cents = (int)(pp->retailprice % 100);

	assert(pp->partkey >= 0);

	b = buffer + sprintf(buffer, HUGE_FORMAT "|%s|%s|%s|%s|" HUGE_FORMAT "|%s|%d.%02d|%s\n", 
		pp->partkey,pp->name,pp->mfgr,pp->brand,pp->type, pp->size,pp->container,dollars,cents,pp->comment);




	if (PQputCopyData(part_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(part_conn));
		PQfinish(part_conn);
		exit(1);
	}


	return(0);
}

int 
ld_psupp (part_t *pp, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[1024];
	static char * b;
	int dollars,
		cents;
	int i;


	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		psupp_conn = connectToDb();





		res = PQexec(psupp_conn, "COPY PARTSUPP FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(psupp_conn));
			PQclear(res);
			PQfinish(psupp_conn);
			exit(1);
		}
		PQclear(res);



	}


	if (numSegs > 0)
	{
		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 
	}


	b = buffer;

	for (i = 0; i < SUPP_PER_PART; i++)
	{
		if (numSegs > 0)
		{
			int segIndex = 0;

			if ( h == NULL )
				h = makeCdbHash(numSegs, HASH_FNV_1); 

			cdbhashinit(h);
			cdbhash(h, (DSS_HUGE )(pp->s[i].partkey)); 

			segIndex = cdbhashreduce(h);
			if ( !segIndexFlags[segIndex] )
				continue;
		}
		dollars = (int)(pp->s[i].scost / 100);
		if (pp->s[i].scost < 0)
			cents = (int) ((- pp->s[i].scost) % 100);
		else
			cents = (int)(pp->s[i].scost % 100);

		assert(pp->s[i].partkey >= 0);
		assert(pp->s[i].suppkey >= 0);


		b = b + sprintf(b, HUGE_FORMAT "|" HUGE_FORMAT "|" HUGE_FORMAT "|%d.%02d|%s\n", 
			pp->s[i].partkey,pp->s[i].suppkey,pp->s[i].qty,dollars,cents,pp->s[i].comment);


	}



	if (b!=buffer)
	if (PQputCopyData(psupp_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(psupp_conn));
		PQfinish(psupp_conn);
		exit(1);
	}


	return(0);

}


int 
hd_supp (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the supplier table\n");

	return(0);
}

int 
ld_supp (supplier_t *supp, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[1024];
	static char * b;
	int dollars,
		cents;



	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		supp_conn = connectToDb();



		res = PQexec(supp_conn, "COPY SUPPLIER FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(supp_conn));
			PQclear(res);
			PQfinish(supp_conn);
			exit(1);
		}
		PQclear(res);



	}


	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(supp->suppkey)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}



	b = buffer;

	dollars = (int)(supp->acctbal / 100);
	if (supp->acctbal < 0)
		cents = (int) ((- supp->acctbal) % 100);
	else
		cents = (int)(supp->acctbal % 100);


	assert(supp->suppkey >= 0);

	b = b + sprintf(b, HUGE_FORMAT "|%s|%s|" HUGE_FORMAT "|%s|%d.%02d|%s\n", 
		supp->suppkey,supp->name,supp->address,supp->nation_code,supp->phone,dollars,cents,supp->comment);



	if (PQputCopyData(supp_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(supp_conn));
		PQfinish(supp_conn);
		exit(1);
	}


	return(0);

}


int 
hd_order (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the order table\n");

	return(0);
}

int 
ld_order (order_t *o, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[512];
	static char * b;
	int dollars,
		cents;
	int i;

	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		
		
		if (isPartitioned == 0)
		{
		order_conn = connectToDb();
		order_part_conn[0] = order_conn;


		res = PQexec(order_conn, "COPY ORDERS FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(order_conn));
			PQclear(res);
			PQfinish(order_conn);
			exit(1);
		}
		PQclear(res);
		}
		else
		{
			int i;
			char copycommand[256];
			
			for (i=0;i<numparts;i++)
				order_part_conn[i] = connectToDb();
				
			for (i=0;i<numparts;i++)
			{
				sprintf(copycommand,
				 "COPY %s%s FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

				, base_orders_name, partnames[i]	);
				res = PQexec(order_part_conn[i],copycommand);
				if (PQresultStatus(res) != PGRES_COPY_IN)
				{
					fprintf(stderr, "COPY failed: %s", PQerrorMessage(order_part_conn[i]));
					PQclear(res);
					PQfinish(order_part_conn[0]);
					exit(1);
				}
				PQclear(res);
			}
			order_conn = order_part_conn[0];
		}



	}


	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(o->okey)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}

	if (isPartitioned)
	{
		order_conn = order_part_conn[0]; 	/* Catch-all partition */
		for (i=0;i<numparts;i++)
		{
			if (strcmp(o->odate , part_start[i]) >= 0 && strcmp(o->odate , part_start[i+1]) < 0)
			{
				order_conn = order_part_conn[i];	
				break;
			}
			
		}
	
		assert(order_conn != 0);
	}

	dollars = (int)(o->totalprice / 100);
	if (o->totalprice < 0)
		cents = (int) ((- o->totalprice) % 100);
	else
		cents = (int)(o->totalprice % 100);

	assert(o->okey >= 0);
	assert(o->custkey >= 0);

	b = buffer + sprintf(buffer, HUGE_FORMAT "|" HUGE_FORMAT "|%c|%d.%02d|%s|%s|%s|%ld|%s\n", 
		o->okey,o->custkey,o->orderstatus,dollars,cents,o->odate,o->opriority,o->clerk,o->spriority,o->comment);


	if (PQputCopyData(order_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(order_conn));
		PQfinish(order_conn);
		exit(1);
	}

	return(0);

}

ld_line (order_t *o, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[5000];
	static char * b;
	int dollars,
		cents;
	int dollars2,
		cents2;
	int dollars3,
		cents3;
	int i;


	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		
		if (isPartitioned == 0)
		{
		line_conn = connectToDb();
		line_part_conn[0] = line_conn;



		res = PQexec(line_conn, "COPY LINEITEM FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(line_conn));
			PQclear(res);
			PQfinish(line_conn);
			exit(1);
		}
		PQclear(res);
		}
		else
		{
			int i;
			char copycommand[256];
			
			for (i=0;i<numparts;i++)
				line_part_conn[i] = connectToDb();
				
			for (i=0;i<numparts;i++)
			{
				sprintf(copycommand,
				 "COPY %s%s FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

				, base_lineitem_name, partnames[i]	);
				res = PQexec(line_part_conn[i],copycommand);
				if (PQresultStatus(res) != PGRES_COPY_IN)
				{
					fprintf(stderr, "COPY failed: %s", PQerrorMessage(line_part_conn[i]));
					PQclear(res);
					PQfinish(line_part_conn[0]);
					exit(1);
				}
				PQclear(res);
			}
			line_conn = line_part_conn[0];
		}
			
 


	}


	if (numSegs > 0)
	{

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 
	}


	b = buffer;

	for (i = 0; i < o->lines; i++)
	{
		int j;
		if (numSegs > 0)
		{
			int segIndex = 0;

			cdbhashinit(h);
			cdbhash(h, (DSS_HUGE )(o->l[i].okey)); 

			segIndex = cdbhashreduce(h);
			if ( !segIndexFlags[segIndex] )
				continue;
		}
		
		
		dollars = (int)(o->l[i].eprice / 100);
		if (o->l[i].eprice < 0)
			cents = (int) ((- o->l[i].eprice) % 100);
		else
			cents = (int)(o->l[i].eprice % 100);

		dollars2 = (int)(o->l[i].discount / 100);
		if (o->l[i].discount < 0)
			cents2 = (int) ((- o->l[i].discount) % 100);
		else
			cents2 = (int)(o->l[i].discount % 100);

		dollars3 = (int)(o->l[i].tax / 100);
		if (o->l[i].tax < 0)
			cents3 = (int) ((- o->l[i].tax) % 100);
		else
			cents3 = (int)(o->l[i].tax % 100);
		/*
		assert(o->l[i].okey >=0);
		assert(o->l[i].partkey >= 0);
		assert(o->l[i].suppkey >= 0);
		assert(cents >= 0);
		assert(cents2 >= 0);
		assert(cents3 >= 0);
		*/
		
		b = b + sprintf(b, HUGE_FORMAT "|" HUGE_FORMAT "|" HUGE_FORMAT "|" HUGE_FORMAT "|" HUGE_FORMAT  		
			"|%d.%02d|%d.%02d|%d.%02d|%c|%c|%s|%s|%s|%s|%s|%s\n", 
			o->l[i].okey,o->l[i].partkey,o->l[i].suppkey,o->l[i].lcnt,o->l[i].quantity,
			dollars,cents,dollars2,cents2,dollars3,cents3,
			o->l[i].rflag[0],o->l[i].lstatus[0],o->l[i].sdate,o->l[i].cdate,o->l[i].rdate,o->l[i].shipinstruct,o->l[i].shipmode,  
			o->l[i].comment);
		if (isPartitioned)
		{
			line_conn = line_part_conn[0]; 	/* Catch-all partition */
			for (j=0;j<numparts;j++)
			{
				if (strcmp(o->l[i].sdate , part_start[j])>=0 && strcmp(o->l[i].sdate , part_start[j+1]) < 0)
				{
					line_conn = line_part_conn[j];	
					break;
				}
				
			}
			assert(line_conn != 0);
			
			if (PQputCopyData(line_conn,
				buffer, b-buffer) != 1)  
			{
				fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(line_conn));
				PQfinish(line_conn);
				exit(1);
			}
			b=buffer;
			
		}
	}

	if (b!=buffer)
	if (PQputCopyData(line_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(line_conn));
		PQfinish(line_conn);
		exit(1);
	}
	 
	return(0);
}



int 
hd_psupp (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("%s %s\n",
		"No header has been defined for the",
		"part supplier table");

	return(0);
}


int 
hd_line (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the lineitem table\n");

	return(0);
}

int 
hd_nation (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the nation table\n");

	return(0);
}

int 
ld_nation (code_t *c, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[256];
	static char * b;


	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		nation_conn = connectToDb();


		res = PQexec(nation_conn, "COPY NATION FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(nation_conn));
			PQclear(res);
			PQfinish(nation_conn);
			exit(1);
		}
		PQclear(res);



	}

	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(c->code)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}



	b = buffer + sprintf(buffer, HUGE_FORMAT "|%s|%ld|%s\n", 
		c->code, c->text,c->join,c->comment); 


	if (PQputCopyData(nation_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(nation_conn));
		PQfinish(nation_conn);
		exit(1);
	}


	return(0);
}

int 
hd_region (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the region table\n");

	return(0);
}

int 
ld_region (code_t *c, int mode)
{
	static int count = 0;
	PGresult   *res = 0;

	static char buffer[256];
	static char * b;



	if (! count++)
	{
		PGresult   *res = 0;
		/* Make a connection to the database */
		region_conn = connectToDb();



		res = PQexec(region_conn, "COPY REGION FROM STDIN "
#ifdef BINARY
			"BINARY"
#else
			"DELIMITER '|'"
#endif

			);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			fprintf(stderr, "COPY failed: %s", PQerrorMessage(region_conn));
			PQclear(res);
			PQfinish(region_conn);
			exit(1);
		}
		PQclear(res);



	}


	if (numSegs > 0)
	{
		int segIndex = 0;

		if ( h == NULL )
			h = makeCdbHash(numSegs, HASH_FNV_1); 

		cdbhashinit(h);
		cdbhash(h, (DSS_HUGE )(c->code)); 

		segIndex = cdbhashreduce(h);
		if ( !segIndexFlags[segIndex] )
			return(0);
	}

	b = buffer + sprintf(buffer, HUGE_FORMAT "|%s|%s\n", 
		c->code, c->text,c->comment) ;


	if (PQputCopyData(region_conn,
		buffer, b-buffer) != 1)  
	{
		fprintf(stderr, "putCOPYData failed: %s", PQerrorMessage(region_conn));
		PQfinish(region_conn);
		exit(1);
	}


	return(0);
}

int 
ld_order_line (order_t *p, int mode)
{
	ld_order(p, mode);
	ld_line (p, mode);

	return(0);
}

int 
hd_order_line (FILE *f)
{
	hd_order(f);
	hd_line (f);

	return(0);
}

int 
ld_part_psupp (part_t *p, int mode)
{
	ld_part(p, mode);
	ld_psupp (p, mode);

	return(0);
}

int 
hd_part_psupp (FILE *f)
{
	hd_part(f);
	hd_psupp(f);

	return(0);
}
