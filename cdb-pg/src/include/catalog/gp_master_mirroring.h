#ifndef _GP_MASTER_MIRRORING_H_
#define _GP_MASTER_MIRRORING_H_

/*
 * The CATALOG definition has to refer to the type of log_time as
 * "timestamptz" (lower case) so that bootstrap mode recognizes it.  But
 * the C header files define this type as TimestampTz.	Since the field is
 * potentially-null and therefore can't be accessed directly from C code,
 * there is no particular need for the C struct definition to show the
 * field type as TimestampTz --- instead we just make it Datum.
 */

#define timestamptz Datum
/*
 * Defines for gp_master_mirroring table
 */
#define GpMasterMirroringRelationName		"gp_master_mirroring"

#define GpMasterMirroringRelationId 5008

CATALOG(gp_master_mirroring,5008) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	text		summary_state;
	text		detail_state;
	timestamptz	log_time;
	text		error_message;
} FormData_gp_master_mirroring;

#undef timestamptz

#define Natts_gp_master_mirroring				4
#define Anum_gp_master_mirroring_summary_state		1
#define Anum_gp_master_mirroring_detail_state		2
#define Anum_gp_master_mirroring_log_time		    3
#define Anum_gp_master_mirroring_error_message		4
 
typedef FormData_gp_master_mirroring *Form_gp_master_mirroring;

/* ----------------
 *		initial contents of gp_master_mirroring
 * ----------------
 */
DATA(insert (  "Not Configured", _null_, _null_, _null_ ));
DESCR("");

#endif /*_GP_MASTER_MIRRORING_H_*/
