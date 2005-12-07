#ifndef _MPP_MPP_H_
#define _MPP_MPP_H_


/*
 * Defines for mpp.mpp table
 */
#define MppMppRelationName			"mpp_mpp"

#define MppMppRelationId	5001

CATALOG(mpp_mpp,5001) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	NameData	mppname;
	int2		numsegments;
	int2		dbid;
	int2		content;
} FormData_mpp_mpp;

#define Natts_mpp_mpp				4
#define Anum_mpp_mpp_mppname		1
#define Anum_mpp_mpp_numsegments	2
#define Anum_mpp_mpp_dbid			3
#define Anum_mpp_mpp_content		4

#endif /*_MPP_MPP_H_*/
