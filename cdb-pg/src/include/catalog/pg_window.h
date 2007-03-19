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

/* No window functions prior to release 3.0 -- bch 3/17/07 */

#endif   /* _PG_WINDOW_H */
