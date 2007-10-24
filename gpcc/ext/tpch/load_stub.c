/*
* $Id$
*
* Revision History
* ===================
* $Log: load_stub.c,v $
* Revision 1.1  2007/10/24 20:25:23  cktan
* new
*
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
hd_part (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the part table\n");

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
hd_order (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the order table\n");

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
hd_region (FILE *f)
{
	static int count = 0;

	if (! count++)
		printf("No header has been defined for the region table\n");

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
hd_part_psupp (FILE *f)
{
	hd_part(f);
	hd_psupp(f);

	return(0);
}
