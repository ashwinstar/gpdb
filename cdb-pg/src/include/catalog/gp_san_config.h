#ifndef _GP_SAN_CONFIG_H_
#define _GP_SAN_CONFIG_H_

/*
 * Defines for gp_fault_strategy table.
 */
#define GpFaultStrategy		"gp_fault_strategy"

#define GpFaultStrategyId 5039

CATALOG(gp_fault_strategy,5039) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	char		fault_strategy;
} FormData_gp_fault_strategy;

#define Natts_gp_fault_strategy					1
#define Anum_gp_fault_strategy_fault_strategy	1

typedef FormData_gp_fault_strategy *Form_gp_fault_strategy;

/*
 * Defines for gp_san_config table.
 *
 * Stores info for failover using SAN.
 */
#define GpSanConfigRelationName		"gp_san_config"

#define GpSanConfigRelationId 5035

CATALOG(gp_san_config,5035) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	int2		mountid;
	char		active_host;
	char		san_type;

	text		primary_host;
	text		primary_mountpoint;
	text		primary_device;

	text		mirror_host;
	text		mirror_mountpoint;
	text		mirror_device;
} FormData_gp_san_config;

#define Natts_gp_san_config						9
#define Anum_gp_san_config_mountid				1
#define Anum_gp_san_config_active_host			2
#define Anum_gp_san_config_san_type				3
#define Anum_gp_san_config_primary_host			4
#define Anum_gp_san_config_primary_mountpoint	5
#define Anum_gp_san_config_primary_device		6
#define Anum_gp_san_config_mirror_host			7
#define Anum_gp_san_config_mirror_mountpoint	8
#define Anum_gp_san_config_mirror_device		9

typedef FormData_gp_san_config *Form_gp_san_config;

#endif /*_GP_SAN_CONFIG_H_*/
