/*-------------------------------------------------------------------------
 *
 * pg_window.h
 *	  definition of the system "window" relation (pg_window)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 2007, greenplum inc *
 *
 * IDENTIFICATION
 *	  $Id$
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_WINDOW_H
#define _PG_WINDOW_H

#include "nodes/pg_list.h"

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------------------------------------------------------
 *		pg_window definition.
 *
 *		cpp turns this into typedef struct FormData_pg_window
 *
 *	winfnoid			pg_proc OID of the window function itself
 *  winrequireorder     does wf require order by?
 *  winallowframe       does wf allow framing?
 *  winpeercount        does wf definition need peer row count?
 *  wincount            does wf definition need partition row count?
 *  winfunc             immediate function (0 if none)
 *  winprefunc          preliminary function (0 if none)
 *  winpretype          type of preliminary function result 
 *  winfinfunc          final function (0 if none)
 * ----------------------------------------------------------------
 */
#define WindowRelationId  5004

CATALOG(pg_window,5004) BKI_WITHOUT_OIDS
{
	regproc		winfnoid;
	bool		winrequireorder;
	bool		winallowframe;
	bool		winpeercount;
	bool		wincount;
	regproc		winfunc;
	regproc		winprefunc;
	Oid			winpretype;
	regproc		winfinfunc;
} FormData_pg_window;

/* ----------------
 *		Form_pg_window corresponds to a pointer to a tuple with
 *		the format of pg_window relation.
 * ----------------
 */
typedef FormData_pg_window *Form_pg_window;

/* ----------------
 *		compiler constants for pg_window
 * ----------------
 */

#define Natts_pg_window				9
#define Anum_pg_window_winfnoid		1
#define Anum_pg_window_winrequireorder	2
#define Anum_pg_window_winallowframe	3
#define Anum_pg_window_winpeercount	4
#define Anum_pg_window_wincount		5
#define Anum_pg_window_winfunc			6
#define Anum_pg_window_winprefunc		7
#define Anum_pg_window_winpretype		8
#define Anum_pg_window_winfinfunc		9


/* ----------------
 * initial contents of pg_window
 * ---------------
 */
/*							winfunc		winprefunc	winpretype	winfinfunc */

/* row_number */
DATA(insert ( 7000	f f f f	7100		0			0			0 ));

/* rank */
DATA(insert ( 7001	t f f f	7101		0			0			0 ));

/* dense_rank */
DATA(insert ( 7002	t f f f	7102		0			0			0 ));

/* percent_rank */
DATA(insert ( 7003	t f f t	0			7101		20			7303 ));

/* cume_dist */
DATA(insert ( 7004	t f t t	0			7204		20			7304 ));

/* ntile(int) */
DATA(insert ( 7005	t f f t	0			7205		1016		7305 ));

/* ntile(bigint) */
DATA(insert ( 7006	t f f t	0			7206		1016		7305 ));

/* ntile(numeric) */
DATA(insert ( 7007	t f f t	0			7207		1016		7305 ));

/* ratio_to_report */
/* XXX omitted for now; no way to represent auxiliary aggregate SUM(x). */

/* first_value */
/* DATA(insert ( 70xx	t f f f	0				0			0			0 )); */
/* ... for many datatypes */

/* last_value */
/* DATA(insert ( 70xx	t f f f	0				0			0			0 )); */
/* ... for many datatypes */

/* lag */
/* DATA(insert ( 70xx	t f f f	0				0			0			0 )); */
/* ... for many datatypes */

/* lead */
/* DATA(insert ( 70xx	t f f f	0				0			0			0 )); */
/* ... for many datatypes */


/*
 * prototypes for functions in pg_window.c
 */


#endif   /* _PG_WINDOW_H */
