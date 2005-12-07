#ifndef _MPP_CONFIGURATION_H_
#define _MPP_CONFIGURATION_H_

/*
 * Defines for mpp.configuration table
 */
#define MppConfigurationRelationName		"mpp_configuration"
//#define MppConfigurationPrimaryKeyIndexName	"mpp_configuration_pkey"

#define MppConfigurationRelationId 5000

CATALOG(mpp_configuration,5000) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	int2		content;
	bool		definedprimary;
	int2		dbid;
	bool		isprimary;
	bool		valid;
	NameData	hostname;
	int4		port;
	text		datadir;
} FormData_mpp_configuration;

#define Natts_mpp_configuration				8
#define Anum_mpp_configuration_content		1
#define Anum_mpp_configuration_definedprimary		2
#define Anum_mpp_configuration_dbid		3
#define Anum_mpp_configuration_isprimary		4
#define Anum_mpp_configuration_valid		5
#define Anum_mpp_configuration_hostname		6
#define Anum_mpp_configuration_port		7
#define Anum_mpp_configuration_datadir		8
 
 
typedef FormData_mpp_configuration *Form_mpp_configuration;


#endif /*_MPP_CONFIGURATION_H_*/
