#ifndef _GP_CONFIGURATION_H_
#define _GP_CONFIGURATION_H_

/*
 * Defines for gp_configuration table
 */
#define GpConfigurationRelationName		"gp_configuration"

#define GpConfigurationRelationId 5000

CATALOG(gp_configuration,5000) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	int2		content;
	bool		definedprimary;
	int2		dbid;
	bool		isprimary;
	bool		valid;
	NameData	hostname;
	int4		port;
	text		datadir;
} FormData_gp_configuration;

#define Natts_gp_configuration				8
#define Anum_gp_configuration_content		1
#define Anum_gp_configuration_definedprimary		2
#define Anum_gp_configuration_dbid		3
#define Anum_gp_configuration_isprimary		4
#define Anum_gp_configuration_valid		5
#define Anum_gp_configuration_hostname		6
#define Anum_gp_configuration_port		7
#define Anum_gp_configuration_datadir		8
 
 
typedef FormData_gp_configuration *Form_gp_configuration;


#endif /*_GP_CONFIGURATION_H_*/
