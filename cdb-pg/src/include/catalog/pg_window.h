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

DATA(insert ( 7011	t f f f	7106		0		0		0 ));
DATA(insert ( 7074	t f f f	7105		0		0		0 ));
DATA(insert ( 7075	t f f f	7104		0		0		0 ));
DATA(insert ( 7310  t t f f 7311		0		0		0 ));
DATA(insert ( 7312  t t f f 7313		0		0		0 ));
DATA(insert ( 7314  t t f f 7315		0		0		0 ));
DATA(insert ( 7316  t t f f 7317		0		0		0 ));
DATA(insert ( 7318  t t f f 7319		0		0		0 ));
DATA(insert ( 7320  t t f f 7321		0		0		0 ));
DATA(insert ( 7322  t t f f 7323		0		0		0 ));
DATA(insert ( 7324  t t f f 7325		0		0		0 ));
DATA(insert ( 7326  t t f f 7327		0		0		0 ));
DATA(insert ( 7328  t t f f 7329		0		0		0 ));
DATA(insert ( 7330  t t f f 7331		0		0		0 ));
DATA(insert ( 7332  t t f f 7333		0		0		0 ));
DATA(insert ( 7334  t t f f 7335		0		0		0 ));
DATA(insert ( 7336  t t f f 7337		0		0		0 ));
DATA(insert ( 7338  t t f f 7339		0		0		0 ));
DATA(insert ( 7340  t t f f 7341		0		0		0 ));
DATA(insert ( 7342  t t f f 7343		0		0		0 ));
DATA(insert ( 7344  t t f f 7345		0		0		0 ));
DATA(insert ( 7346  t t f f 7347		0		0		0 ));
DATA(insert ( 7348  t t f f 7349		0		0		0 ));
DATA(insert ( 7350  t t f f 7351		0		0		0 ));
DATA(insert ( 7352  t t f f 7353		0		0		0 ));
DATA(insert ( 7354  t t f f 7355		0		0		0 ));
DATA(insert ( 7356  t t f f 7357		0		0		0 ));
DATA(insert ( 7358  t t f f 7359		0		0		0 ));
DATA(insert ( 7360  t t f f 7361		0		0		0 ));
DATA(insert ( 7362  t t f f 7363		0		0		0 ));
DATA(insert ( 7364  t t f f 7365		0		0		0 ));
DATA(insert ( 7366  t t f f 7367		0		0		0 ));
DATA(insert ( 7368  t t f f 7369		0		0		0 ));
DATA(insert ( 7370  t t f f 7371		0		0		0 ));
DATA(insert ( 7372  t t f f 7373		0		0		0 ));
DATA(insert ( 7374  t t f f 7375		0		0		0 ));
DATA(insert ( 7376  t t f f 7377		0		0		0 ));
DATA(insert ( 7378  t t f f 7379		0		0		0 ));
DATA(insert ( 7380  t t f f 7381		0		0		0 ));
DATA(insert ( 7382  t t f f 7383		0		0		0 ));
DATA(insert ( 7384  t t f f 7385		0		0		0 ));
DATA(insert ( 7386  t t f f 7387		0		0		0 ));
DATA(insert ( 7388  t t f f 7389		0		0		0 ));
DATA(insert ( 7390  t t f f 7391		0		0		0 ));
DATA(insert ( 7392  t t f f 7393		0		0		0 ));
DATA(insert ( 7394  t t f f 7395		0		0		0 ));
DATA(insert ( 7396  t t f f 7397		0		0		0 ));
DATA(insert ( 7398  t t f f 7399		0		0		0 ));
DATA(insert ( 7400  t t f f 7401		0		0		0 ));
DATA(insert ( 7402  t t f f 7403		0		0		0 ));
DATA(insert ( 7404  t t f f 7405		0		0		0 ));
DATA(insert ( 7406  t t f f 7407		0		0		0 ));
DATA(insert ( 7408  t t f f 7409		0		0		0 ));
DATA(insert ( 7410  t t f f 7411		0		0		0 ));
DATA(insert ( 7412  t t f f 7413		0		0		0 ));
DATA(insert ( 7414  t t f f 7415		0		0		0 ));
DATA(insert ( 7416  t t f f 7417		0		0		0 ));
DATA(insert ( 7418  t t f f 7419		0		0		0 ));
DATA(insert ( 7420  t t f f 7421		0		0		0 ));
DATA(insert ( 7422  t t f f 7423		0		0		0 ));
DATA(insert ( 7424  t t f f 7425		0		0		0 ));
DATA(insert ( 7426  t t f f 7427		0		0		0 ));
DATA(insert ( 7428  t t f f 7429		0		0		0 ));
DATA(insert ( 7430  t t f f 7431		0		0		0 ));
DATA(insert ( 7432  t t f f 7433		0		0		0 ));
DATA(insert ( 7434  t t f f 7435		0		0		0 ));
DATA(insert ( 7436  t t f f 7437		0		0		0 ));
DATA(insert ( 7438  t t f f 7439		0		0		0 ));
DATA(insert ( 7440  t t f f 7441		0		0		0 ));
DATA(insert ( 7442  t t f f 7443		0		0		0 ));
DATA(insert ( 7444  t t f f 7445		0		0		0 ));
DATA(insert ( 7446  t t f f 7447		0		0		0 ));
DATA(insert ( 7448  t t f f 7449		0		0		0 ));
DATA(insert ( 7450  t t f f 7451		0		0		0 ));
DATA(insert ( 7452  t t f f 7453		0		0		0 ));
DATA(insert ( 7454  t t f f 7455		0		0		0 ));
DATA(insert ( 7456  t t f f 7457		0		0		0 ));
DATA(insert ( 7458  t t f f 7459		0		0		0 ));
DATA(insert ( 7460  t t f f 7461		0		0		0 ));
DATA(insert ( 7462  t t f f 7463		0		0		0 ));
DATA(insert ( 7464  t t f f 7465		0		0		0 ));
DATA(insert ( 7466  t t f f 7467		0		0		0 ));
DATA(insert ( 7468  t t f f 7469		0		0		0 ));
DATA(insert ( 7470  t t f f 7471		0		0		0 ));
DATA(insert ( 7472  t t f f 7473		0		0		0 ));
DATA(insert ( 7474  t t f f 7475		0		0		0 ));
DATA(insert ( 7476  t t f f 7477		0		0		0 ));
DATA(insert ( 7478  t t f f 7479		0		0		0 ));
DATA(insert ( 7480  t t f f 7481		0		0		0 ));
DATA(insert ( 7482  t t f f 7483		0		0		0 ));
DATA(insert ( 7484  t t f f 7485		0		0		0 ));
DATA(insert ( 7486  t t f f 7487		0		0		0 ));
DATA(insert ( 7488  t t f f 7489		0		0		0 ));
/* ... for many datatypes */


/*
 * prototypes for functions in pg_window.c
 */


#endif   /* _PG_WINDOW_H */
