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

/*
 * Defines for gp_configuration_history table
 * 
 * Used by fault-management components to record a "change history" description
 * with timestamp.
 */

/*
 * The CATALOG definition has to refer to the type of log_time as
 * "timestamptz" (lower case) so that bootstrap mode recognizes it.  But
 * the C header files define this type as TimestampTz.	Since the field is
 * potentially-null and therefore can't be accessed directly from C code,
 * there is no particular need for the C struct definition to show the
 * field type as TimestampTz --- instead we just make it Datum.
 */
#define timestamptz Datum

#define GpConfigHistoryRelationName		"gp_configuration_history"

#define GpConfigHistoryRelationId 5006

CATALOG(gp_configuration_history,5006) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	timestamptz	time;
	int2		dbid;
	text		desc;
} FormData_gp_configuration_history;

#define Natts_gp_configuration_history		3
#define Anum_gp_configuration_history_time	1
#define Anum_gp_configuration_history_dbid	2	
#define Anum_gp_configuration_history_desc	3
 
typedef FormData_gp_configuration_history *Form_gp_configuration_history;

#endif /*_GP_CONFIGURATION_H_*/
