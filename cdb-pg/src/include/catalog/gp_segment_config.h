#ifndef _GP_SEGMENT_CONFIG_H_
#define _GP_SEGMENT_CONFIG_H_

/*
 * Defines for gp_segment_config table
 */
#define GpSegmentConfigRelationName		"gp_segment_config"

#define GpSegmentConfigRelationId 5036

CATALOG(gp_segment_config,5036) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	int2		dbid;			/* up to 32767 databases */
	int2		content;		/* up to 32767 contents -- only 16384 usable with mirroring (see dbid) */

	char		role;
	char		preferred_role;
	char		mode;
	char		status;

	NameData	hostname;
	NameData	address;

	int4		port;
	text		datadir;

	int4		replication_port;
	int2vector	san_mounts; /* one or more mount-points used by this segment. */
} FormData_gp_segment_config;

#define Natts_gp_segment_config					12
#define Anum_gp_segment_config_dbid				1
#define Anum_gp_segment_config_content			2
#define Anum_gp_segment_config_role				3
#define Anum_gp_segment_config_preferred_role	4
#define Anum_gp_segment_config_mode				5
#define Anum_gp_segment_config_status			6
#define Anum_gp_segment_config_hostname			7
#define Anum_gp_segment_config_address			8
#define Anum_gp_segment_config_port				9
#define Anum_gp_segment_config_datadir			10
#define Anum_gp_segment_config_replication_port	11
#define Anum_gp_segment_config_san_mounts		12

typedef FormData_gp_segment_config *Form_gp_segment_config;

#endif /*_GP_SEGMENT_CONFIG_H_*/
