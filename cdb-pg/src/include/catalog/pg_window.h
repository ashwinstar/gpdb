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
DATA(insert ( 7017  t t f f 7111        0       0       0 ));
DATA(insert ( 7018  t t f f 7112		0		0		0 ));
DATA(insert ( 7019  t t f f 7113		0		0		0 ));
DATA(insert ( 7020  t t f f 7114		0		0		0 ));
DATA(insert ( 7021  t t f f 7115		0		0		0 ));
DATA(insert ( 7022  t t f f 7116		0		0		0 ));
DATA(insert ( 7023  t t f f 7117		0		0		0 ));
DATA(insert ( 7024  t t f f 7118		0		0		0 ));
DATA(insert ( 7025  t t f f 7119		0		0		0 ));
DATA(insert ( 7026  t t f f 7120		0		0		0 ));
DATA(insert ( 7027  t t f f 7121		0		0		0 ));
DATA(insert ( 7028  t t f f 7122		0		0		0 ));
DATA(insert ( 7029  t t f f 7123		0		0		0 ));
DATA(insert ( 7030  t t f f 7124		0		0		0 ));
DATA(insert ( 7031  t t f f 7125		0		0		0 ));
DATA(insert ( 7032  t t f f 7126		0		0		0 ));
DATA(insert ( 7033  t t f f 7127		0		0		0 ));
DATA(insert ( 7034  t t f f 7128		0		0		0 ));
DATA(insert ( 7035  t t f f 7129		0		0		0 ));
DATA(insert ( 7036  t t f f 7130		0		0		0 ));
DATA(insert ( 7037  t t f f 7131		0		0		0 ));
DATA(insert ( 7038  t t f f 7132		0		0		0 ));
DATA(insert ( 7039  t t f f 7133		0		0		0 ));
DATA(insert ( 7040  t t f f 7134		0		0		0 ));
DATA(insert ( 7041  t t f f 7135		0		0		0 ));
DATA(insert ( 7042  t t f f 7136		0		0		0 ));
DATA(insert ( 7043  t t f f 7137		0		0		0 ));
DATA(insert ( 7044  t t f f 7138		0		0		0 ));
DATA(insert ( 7045  t t f f 7139		0		0		0 ));
DATA(insert ( 7046  t t f f 7140		0		0		0 ));
DATA(insert ( 7047  t t f f 7141		0		0		0 ));
/* last_value */
DATA(insert ( 7012	t t f f	7103		0		0		0 ));
DATA(insert ( 7013	t t f f	7107		0		0		0 ));
DATA(insert ( 7014	t t f f	7108		0		0		0 ));
DATA(insert ( 7015	t t f f	7109		0		0		0 ));
DATA(insert ( 7016	t t f f	7110		0		0		0 ));
DATA(insert ( 7063  t t f f 7165		0		0		0 ));
DATA(insert ( 7072  t t f f 7166		0		0		0 ));
DATA(insert ( 7073  t t f f 7167		0		0		0 ));
DATA(insert ( 7048  t t f f 7168		0		0		0 ));
DATA(insert ( 7049  t t f f 7142		0		0		0 ));
DATA(insert ( 7050  t t f f 7143		0		0		0 ));
DATA(insert ( 7051  t t f f 7144		0		0		0 ));
DATA(insert ( 7052  t t f f 7145		0		0		0 ));
DATA(insert ( 7053  t t f f 7146		0		0		0 ));
DATA(insert ( 7054  t t f f 7147		0		0		0 ));
DATA(insert ( 7055  t t f f 7148		0		0		0 ));
DATA(insert ( 7056  t t f f 7149		0		0		0 ));
DATA(insert ( 7057  t t f f 7150		0		0		0 ));
DATA(insert ( 7058  t t f f 7151		0		0		0 ));
DATA(insert ( 7059  t t f f 7152		0		0		0 ));
DATA(insert ( 7060  t t f f 7153		0		0		0 ));
DATA(insert ( 7061  t t f f 7154		0		0		0 ));
DATA(insert ( 7062  t t f f 7155		0		0		0 ));
DATA(insert ( 7064  t t f f 7157		0		0		0 ));
DATA(insert ( 7065  t t f f 7158		0		0		0 ));
DATA(insert ( 7066  t t f f 7159		0		0		0 ));
DATA(insert ( 7067  t t f f 7160		0		0		0 ));
DATA(insert ( 7068  t t f f 7161		0		0		0 ));
DATA(insert ( 7069  t t f f 7162		0		0		0 ));
DATA(insert ( 7070  t t f f 7163		0		0		0 ));
DATA(insert ( 7071  t t f f 7164		0		0		0 ));
/* ... for many datatypes */

/* lag */
/* DATA(insert ( 70xx	t f f f	0				0			0			0 )); */
/* ... for many datatypes */

/* lead */
#define LEAD_WINDOW_FUNC_OID 7011
DATA(insert ( 7011	t f f f	7106		0			0			0 ));
/* ... for many datatypes */


/*
 * prototypes for functions in pg_window.c
 */


#endif   /* _PG_WINDOW_H */
