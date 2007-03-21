#ifndef _GP_GP_H_
#define _GP_GP_H_


/*
 * Defines for gp_gp table
 */
#define GpGpRelationName			"gp_gp"

#define GpGpRelationId	5001

CATALOG(gp_gp,5001) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	NameData	mppname;
	int2		numsegments;
	int2		dbid;
	int2		content;
} FormData_gp_gp;

#define Natts_gp_gp				4
#define Anum_gp_gp_mppname		1
#define Anum_gp_gp_numsegments	2
#define Anum_gp_gp_dbid			3
#define Anum_gp_gp_content		4

#endif /*_GP_GP_H_*/
