#ifndef _MPP_VERSION_H_
#define _MPP_VERSION_H_

/*
 * Defines for mpp_version table
 */
#define MppVersionRelationName		"mpp_version_at_initdb"

CATALOG(mpp_version_at_initdb,5003) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	int2		schemaversion;
	text		productversion;
} FormData_mpp_version;

#define Natts_mpp_version				2
#define Anum_mpp_version_schemaversion		1
#define Anum_mpp_version_productversion		2

DATA(insert ( 2  "$$Name:  $$" ));

#endif /*_MPP_VERSION_H_*/
