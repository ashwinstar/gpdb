#ifndef _GP_ID_H_
#define _GP_ID_H_


/*
 * Defines for gp_id table
 */
#define GpIdRelationName			"gp_id"

#define GpIdRelationId	5001

CATALOG(gp_id,5001) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	NameData	gpname;
	int2		numsegments;
	int2		dbid;
	int2		content;
} FormData_gp_id;

#define Natts_gp_id				4
#define Anum_gp_id_gpname		1
#define Anum_gp_id_numsegments	2
#define Anum_gp_id_dbid			3
#define Anum_gp_id_content		4

#endif /*_GP_ID_H_*/
