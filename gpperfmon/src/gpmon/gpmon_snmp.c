#include "gpmonlib.h"
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#define GPMMON_SNMP_COMMUNITY_NAME "public"
#define GPMMON_SNMP_PROTOCOL_VERSION SNMP_VERSION_2c
#define GPMMON_SNMP_OID_STRING_BUF_SIZE 512
#define GPMON_SNMP_ALLOW_REALLOC 0
#define GPMON_SNMP_CONFIG_MAX_STRING_LENGTH 128


// these are defined in order of good to bad
typedef enum GPDB_SNMP_STATUS_T
{
	GPDB_SNMP_STATUS_NORMAL=0,
	GPDB_SNMP_STATUS_UNKNOWN=1,
	GPDB_SNMP_STATUS_WARNING=2,
	GPDB_SNMP_STATUS_ERROR=3,
	GPDB_SNMP_STATUS_UNREACHABLE=4
} GPDB_SNMP_STATUS;

const char* snmp_status_prt(GPDB_SNMP_STATUS status)
{
	switch(status)
	{
		case GPDB_SNMP_STATUS_NORMAL:
			return "normal";
		case GPDB_SNMP_STATUS_WARNING:
			return "warning";
		case GPDB_SNMP_STATUS_ERROR:
			return "error";
		case GPDB_SNMP_STATUS_UNREACHABLE:
			return "unreachable";
		case GPDB_SNMP_STATUS_UNKNOWN:
		default:
			return "unknown";
	}
}

typedef void gpdb_snmp_format_function(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars);
typedef GPDB_SNMP_STATUS gpdb_status_code_conversion_function(int statusCodeIn, char** msgout);
typedef void gpdb_snmp_device_function(const char* category, const char* hostname);

struct SnmpConfigEntry
{
	char category[GPMON_SNMP_CONFIG_MAX_STRING_LENGTH];
	char hostname[GPMON_SNMP_CONFIG_MAX_STRING_LENGTH];
	gpdb_snmp_device_function* function;

	// linked list
	struct SnmpConfigEntry* next;
};

static struct 
{
	unsigned char snmpInitialized;
	struct SnmpConfigEntry* snmpConfigHead;
	struct SnmpConfigEntry* snmpConfigTail;
	char outputdir[PATH_MAX];
	char lastreport_file[PATH_MAX];
	char lastreport_tmpfile[PATH_MAX];
	char hostlist_file[PATH_MAX];
	char hostlist_tmpfile[PATH_MAX];
	FILE* hostlist_fd;

	time_t lastreport_started;
	time_t lastreport_done;

	// these are per host vars reset for each host
	GPDB_SNMP_STATUS worst_status_per_host;

} si = { 0 };

void init_per_host_status()
{
	si.worst_status_per_host = GPDB_SNMP_STATUS_NORMAL;
}

void update_per_host_status(GPDB_SNMP_STATUS componentStatus)
{
	if (componentStatus > si.worst_status_per_host)
		si.worst_status_per_host = componentStatus;
}

FILE* snmp_open_hostlist_file()
{
	if (si.hostlist_fd)
	{
		gpmon_warningx(FLINE, 0, "attempt to reopen already opened snmp hostlist file not expected");
	}

	si.hostlist_fd = fopen(si.hostlist_tmpfile, "w");
	return si.hostlist_fd;
}

void write_line_in_hostlist_file(const char* category, const char* hostname)
{
	if (si.hostlist_fd)
	{
		fprintf(si.hostlist_fd, "%s|%s|%s\n", category, hostname, snmp_status_prt(si.worst_status_per_host));
	}
	else
	{
		gpmon_warningx(FLINE, 0, "unexpected closed hostlist file\n");
	}
}

void snmp_close_hostlist_file()
{
	if (si.hostlist_fd)
	{
		fclose(si.hostlist_fd);
		si.hostlist_fd = NULL;
		rename(si.hostlist_tmpfile, si.hostlist_file);
	}
	else
	{
		gpmon_warningx(FLINE, 0, "attempt to close non-open snmp hostlist file not expected");
	}
}

void addSnmpConfigEntry(const char* category, const char* host, gpdb_snmp_device_function* function)
{
	struct SnmpConfigEntry* newEntry = NULL;
	newEntry = malloc(sizeof(struct SnmpConfigEntry));
	memset(newEntry, 0, sizeof(struct SnmpConfigEntry));
	snprintf(newEntry->category, GPMON_SNMP_CONFIG_MAX_STRING_LENGTH, "%s", category);
	snprintf(newEntry->hostname, GPMON_SNMP_CONFIG_MAX_STRING_LENGTH, "%s", host);
	newEntry->function = function;

	if (si.snmpConfigTail)
	{
		si.snmpConfigTail->next = newEntry;
		si.snmpConfigTail = newEntry;
	}
	else
	{
		si.snmpConfigHead = si.snmpConfigTail = newEntry;
	}
}

GPDB_SNMP_STATUS brocade_switch_status_code_conversion(int statusCodeIn, char** msgout)
{
	//1. up
	//2. down
	//3. testing
	//4. faulty
	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "";
		break;

		case 2:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "down";
		break;

		case 3:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "testing";
		break;

		case 4:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "faulty";
		break;

		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected status from device";
		break;
	}
	return status;
}

GPDB_SNMP_STATUS snmp_network_interface_status_code_conversion(int statusCodeIn, char** msgout)
{
	//1. up
	//2. down
	//3. testing
	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "up";
		break;

		case 2:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "down";
		break;

		case 3:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "testing";
		break;

		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected status from device";
		break;
	}
	return status;
}

GPDB_SNMP_STATUS dell_network_device_connection_status_conversion(int statusCodeIn, char** msgout)
{
    //unknown(0) Unable to determine connection status. 
    //connected(1) Media reports that device is connected. 
    //disconnected(2) Media reports that device is disconnected. 
    //driverBad(3) Driver cannot be opened to determine status. 
    //driverDisabled(4) Driver is disabled. 
    //hardwareInitalizing(10) Hardware is initializing. 
    //hardwareResetting(11) Hardware is resetting. 
    //hardwareClosing(12) Hardware is closing down. 
    //hardwareNotReady(13) Hardware is not ready. 

	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 0:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unknown";
        break;

		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "connected";
        break;

		case 2:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "disconnected";
        break;

		case 3:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "driver bad";
        break;

		case 4:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "driver disabled";
        break;

		case 10:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "hardware initalizing";
        break;

		case 11:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "hardware resetting";
        break;

		case 12:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "hardware closing";
        break;

		case 13:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "hardware not ready";
        break;

		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}
	return status;
}

GPDB_SNMP_STATUS dell_status_code_conversion(int statusCodeIn, char** msgout)
{
		//other(1) The object’s status is not one of the following: 
		//unknown(2) The object’s status is unknown. 
		//ok(3) The object’s status is OK. 
		//nonCritical(4) The object’s status is warning, noncritical. 
		//critical(5) The object’s status is critical (failure). 
		//nonRecoverable(6) The object’s status is nonrecoverable (dead). :
	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "other";
		break;
		case 2:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unknown";
		break;
		case 3:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "";
		break;
		case 4:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "nonCritical";
		break;

		case 5:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "critical";
		break;

		case 6:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "nonRecoverable";
		break;

		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected status from device";
		break;
	}
	return status;
}

GPDB_SNMP_STATUS dell_controller_status_code_conversion(int statusCodeIn, char** msgout)
{
	//0: Unknown 
	//1: Ready 
	//2: Failed 
	//3: Online 
	//4: Offline 
	//6: Degraded 

	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 0:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unknown";
		break;
		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;;
			*msgout = "";
		break;
		case 2:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Failed";
		break;
		case 3:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "";
		break;
		case 4:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Offline";
		break;
		case 6:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Degraded";
		break;
		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}

	return status;
}

GPDB_SNMP_STATUS dell_virtual_disk_state_status_code_conversion(int statusCodeIn, char** msgout)
{
	//0: Unknown 
	//1: Ready - The disk is accessible and has no known problems. 
	//2: Failed - The data on the virtual disk is no longer fault tolerant because one of the underlying disks is not online. 
	//3: Online 
	//4: Offline - The disk is not accessible. The disk may be corrupted or intermittently unavailable. 
	//6: Degraded - The data on the virtual disk is no longer fault tolerant because one of the underlying disks is not online. 
	//15: Resynching 
	//16: Regenerating 
	//24: Rebuilding 
	//26: Formatting 
	//32: Reconstructing 
	//35: Initializing 
	//36: Background Initialization 
	//38: Resynching Paused 
	//52: Permanently Degraded 
	//54: Degraded Redundancy 

	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 0:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unknown";
		break;
		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "Ready";
		break;
		case 2:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Failed";
		break;
		case 3:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "Online";
		break;
		case 4:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Offline";
		break;
		case 6:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Degraded";
		break;
		case 15:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Resynching";
		break;
		case 16:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Regenerating";
		break;
		case 24:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Rebuilding";
		break;
		case 26:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Formatting";
		break;
		case 32:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Reconstructing";
		break;
		case 35:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Initialization";
		break;
		case 36:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Background Initialization";
		break;
		case 38:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Resynching Paused";
		break;
		case 52:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Permanently Degraded";
		break;
		case 54:
			status = GPDB_SNMP_STATUS_ERROR;
			*msgout = "Degraded Redundancy";
		break;
		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}

	return status;
}


GPDB_SNMP_STATUS dell_virtual_disk_layout_code_conversion(int statusCodeIn, char** msgout)
{
	//1: Concatenated 
	//2: RAID-0 
	//3: RAID-1 
	//7: RAID-5 
	//8: RAID-6 
	//10: RAID-10 
	//12: RAID-50 
	//19: Concatenated RAID 1 
	//24: RAID-60 

	GPDB_SNMP_STATUS status;

	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "Concatenated";
		break;
		case 2:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-0";
		break;
		case 3:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-1";
		break;
		case 7:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-5";
		break;
		case 8:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-6";
		break;
		case 10:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-10";
		break;
		case 12:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-50";
		break;
		case 19:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "Concatenated RAID 1";
		break;
		case 24:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "RAID-60";
		break;
		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}

	return status;
}

GPDB_SNMP_STATUS dell_virtual_disk_read_policy_code_conversion(int statusCodeIn, char** msgout)
{
	GPDB_SNMP_STATUS status;

    //1: Enabled - Adaptec Read Cache Enabled 
    //2: Disabled - Adaptec Read Cache Disabled 
    //3: LSI Read Ahead 
    //4: LSI Adaptive Read Ahead 
    //5: LSI No Read Ahead 

	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Enabled Adaptec Read Cache Enabled";
		break;

		case 2:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Disabled Adaptec Read Cache Disabled";
		break;

		case 3:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "LSI Read Ahead";
		break;

		case 4:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "LSI Adaptive Read Ahead";
		break;

		case 5:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "LSI No Read Ahead";
		break;

		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}

	return status;
}


GPDB_SNMP_STATUS dell_virtual_disk_write_policy_code_conversion(int statusCodeIn, char** msgout)
{
	GPDB_SNMP_STATUS status;

	//1: Enabled - Adaptec Write Cache Enabled Protected 
	//2: Disabled - Adaptec Write Cache Disabled 
	//3: LSI Write Back 
	//4: LSI Write Through 
	//5: Enabled Always - (Adaptec only) 
	//6: Enabled Always - (SAS only) 
	
	switch(statusCodeIn)
	{
		case 1:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Enabled Adaptec Write Cache Enabled Protected";
		break;
		case 2:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Disabled Adaptec Write Cache Disabled";
		break;
		case 3:
			status = GPDB_SNMP_STATUS_NORMAL;
			*msgout = "LSI Write Back";
		break;
		case 4:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "LSI Write Through";
		break;
		case 5:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Enabled Always (Adaptec)";
		break;
		case 6:
			status = GPDB_SNMP_STATUS_WARNING;
			*msgout = "Enabled Always (SAS)";
		break;
		default:
			status = GPDB_SNMP_STATUS_UNKNOWN;
			*msgout = "unexpected code from device";
		break;
	}

	return status;
}

#define FORMATIP_SIZE 64
void gpmon_format_snmp_ip(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	unsigned char *ip;
	char buffer[FORMATIP_SIZE] = { 0 };
	char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
	snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, oidname, oidlen);

	if (vars->type != ASN_IPADDRESS)
	{
		fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNKNOWN), "wrong var type");
		update_per_host_status(GPDB_SNMP_STATUS_UNKNOWN);
		return;
	}
	ip = vars->val.string;
	snprintf(buffer, FORMATIP_SIZE, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
	fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_NORMAL), buffer);
	update_per_host_status(GPDB_SNMP_STATUS_NORMAL);
}

#define FORMAT_SNMP_INT_BUFSIZE 64
void gpmon_format_snmp_int(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
	snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, oidname, oidlen);
	char outbuf[FORMAT_SNMP_INT_BUFSIZE] = { 0 };

	if (vars->type != ASN_INTEGER)
	{
		fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNKNOWN), "wrong var type");
		update_per_host_status(GPDB_SNMP_STATUS_UNKNOWN);
		return;
	}

	snprintf(outbuf, FORMAT_SNMP_INT_BUFSIZE, "%ld", *vars->val.integer);
	fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_NORMAL), outbuf);
	update_per_host_status(GPDB_SNMP_STATUS_NORMAL);
}

void format_generic_status_code(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars, gpdb_status_code_conversion_function conversion_func)
{
	char* msg;
	int status;

	char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
	snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, oidname, oidlen);

	if (vars->type != ASN_INTEGER)
	{
		fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNKNOWN), "wrong var type");
		update_per_host_status(GPDB_SNMP_STATUS_UNKNOWN);
		return;
	}

	status = conversion_func(*vars->val.integer, &msg);

	fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(status), msg);
	update_per_host_status(status);
}

void gpmon_format_dell_status(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_status_code_conversion);
}

void gpmon_format_dell_network_device_status(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_network_device_connection_status_conversion);
}

void format_dell_controller_status(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_controller_status_code_conversion);
}

void format_dell_virtual_disk_layout(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_virtual_disk_layout_code_conversion);
}

void format_dell_virtual_disk_state(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_virtual_disk_state_status_code_conversion);
}

void format_dell_virtual_disk_write_policy(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_virtual_disk_write_policy_code_conversion);
}

void format_dell_virtual_disk_read_policy(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, dell_virtual_disk_read_policy_code_conversion);
}

void format_network_interface_status(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, snmp_network_interface_status_code_conversion);
}

void format_brocade_switch_status(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	format_generic_status_code(fd, hostname, oidname, oidlen, displayname, vars, brocade_switch_status_code_conversion);
}

#define FORMAT_DELL_SCALAR_STRING_BUF_SIZE 256
void gpmon_format_snmp_string(FILE* fd, const char* hostname, oid oidname[], size_t oidlen, const char* displayname, struct variable_list *vars)
{
	// HOST|oid|name|status|value

	char buffer[FORMAT_DELL_SCALAR_STRING_BUF_SIZE] = { 0 };

	char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
	snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, oidname, oidlen);

	if (vars->type != ASN_OCTET_STR)
	{

		fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNKNOWN), "wrong var type");
		update_per_host_status(GPDB_SNMP_STATUS_UNKNOWN);
		return;
	}

	unsigned int snprintf_size = FORMAT_DELL_SCALAR_STRING_BUF_SIZE;
	if (vars->val_len < FORMAT_DELL_SCALAR_STRING_BUF_SIZE)
	{
		snprintf_size = vars->val_len + 1;

		// paranoid check ... should not get here
		if (snprintf_size < 1 || snprintf_size > FORMAT_DELL_SCALAR_STRING_BUF_SIZE)
		{
			gpmon_warningx(FLINE, 0, "buffer calculation warning on %s %s %s\n", hostname, oidstring, displayname);
			snprintf_size = FORMAT_DELL_SCALAR_STRING_BUF_SIZE;
		}
	}

	snprintf(buffer, snprintf_size, (char*)vars->val.string);
	fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_NORMAL), buffer);
	update_per_host_status(GPDB_SNMP_STATUS_NORMAL);
}

apr_status_t snmp_msg_send(const char* hostname, struct snmp_pdu *pdu_to_send, struct snmp_pdu** pdu_response)
{
	struct snmp_session session, *ss = NULL;
	apr_status_t retVal = APR_SUCCESS;
	int status;

	snmp_sess_init( &session );
	session.peername = (char*)hostname;
	session.version = GPMMON_SNMP_PROTOCOL_VERSION;
	session.community = (unsigned char*)GPMMON_SNMP_COMMUNITY_NAME;
	session.community_len = strlen((char*)session.community);
	ss = snmp_open(&session);

	if (!ss)
	{
		gpmon_warningx(FLINE, 0, "snmp_open session failure to host %s\n", hostname);
		return APR_ECONNABORTED;
	}

	status = snmp_synch_response(ss, pdu_to_send, pdu_response);

	if (status == STAT_TIMEOUT)
	{
		retVal = APR_ETIMEDOUT;
	}
	else if (status == STAT_SUCCESS)
	{
		if ((*pdu_response)->errstat == SNMP_ERR_NOSUCHNAME ||
			!((*pdu_response)->variables) ||
			(*pdu_response)->variables->type == SNMP_NOSUCHOBJECT ||
			(*pdu_response)->variables->type == SNMP_NOSUCHINSTANCE ||
			(*pdu_response)->variables->type == SNMP_ENDOFMIBVIEW)
		{
			retVal = APR_NOTFOUND;
		}
		if ((*pdu_response)->errstat != SNMP_ERR_NOERROR)
		{
			gpmon_warningx(FLINE, 0, "snmp_synch_response response has error '%s' from host %s\n", snmp_errstring((*pdu_response)->errstat), hostname);
		}
	}
	else
	{
		gpmon_warningx(FLINE, 0, "snmp_synch_response error %d from host %s\n", status, hostname);
		retVal = APR_EPROC_UNKNOWN;
	}

   	snmp_close(ss);
	return retVal;
}

#define SNMP_ARRAY_DISPLAY_LENGTH 256
int snmp_array_collection(FILE* fd, const char* hostname, const char* oidname, const char* displayname, gpdb_snmp_format_function format_func, bool enumerateDisplay)
{
	apr_status_t retVal;
	oid originalOID[MAX_OID_LEN] = {};
	size_t originalOID_len = MAX_OID_LEN;
	size_t originalOID_comparebytes = 0;
	
	oid nextOID[MAX_OID_LEN] = {};
	size_t nextOID_len;

	char display_buffer[SNMP_ARRAY_DISPLAY_LENGTH];

	read_objid(oidname, originalOID, &originalOID_len);
	originalOID_comparebytes = originalOID_len * sizeof(oid);

	// start with original oid
	memmove(nextOID, originalOID, originalOID_comparebytes);
	nextOID_len = originalOID_len;
	
	while (1)
	{
		struct snmp_pdu *pdu = NULL;
		struct snmp_pdu *response = NULL;
		int last_item_in_subtree = 0;
	
		pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
		snmp_add_null_var(pdu, nextOID, nextOID_len);

		retVal = snmp_msg_send(hostname, pdu, &response);
	
		if (retVal == APR_SUCCESS)
		{
			if (response->variables->name_length < originalOID_len)
			{
				last_item_in_subtree = 1;
			}
			else if (memcmp(originalOID, response->variables->name, originalOID_comparebytes) != 0)
			{
				last_item_in_subtree = 1;
			}
			else
			{
				// append the index into array into the display name
				if (enumerateDisplay)
				{
					snprintf(display_buffer, SNMP_ARRAY_DISPLAY_LENGTH, "%s %ld", displayname, (long)response->variables->name[response->variables->name_length-1]);
				}
				else
				{
					snprintf(display_buffer, SNMP_ARRAY_DISPLAY_LENGTH, "%s", displayname);
				}

				format_func(fd, hostname, response->variables->name, response->variables->name_length, display_buffer, response->variables);
				memmove(nextOID, response->variables->name, response->variables->name_length * sizeof(oid));
				nextOID_len = response->variables->name_length;
			}
		}
		else if (retVal == APR_ETIMEDOUT)
		{
			char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
			snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, nextOID, nextOID_len);
			fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNREACHABLE), "timeout");
			update_per_host_status(GPDB_SNMP_STATUS_UNREACHABLE);
			last_item_in_subtree = 1;
			gpmon_warningx(FLINE, 0, "timeout trying to access snmp data on host %s for oid %s\n", hostname, oidstring);
		}
		else if (retVal == APR_NOTFOUND)
		{
			char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
			snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, nextOID, nextOID_len);
			gpmon_warningx(FLINE, 0, "snmp_array_collection could not find OID %s at host %d\n", oidstring, hostname);
			last_item_in_subtree = 1;
		}
		else if (retVal == APR_EPROC_UNKNOWN)
		{
			char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
			snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, nextOID, nextOID_len);
			gpmon_warningx(FLINE, 0, "snmp_array_collection got unexpected error looking for OID %s at host %d\n", oidstring, hostname);
			last_item_in_subtree = 1;
		}
		else if (retVal == APR_ECONNABORTED)
		{
			char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
			snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, nextOID, nextOID_len);
			fprintf(fd, "%s|%s|%s|%s|%s\n", hostname, oidstring, displayname, snmp_status_prt(GPDB_SNMP_STATUS_UNREACHABLE), "could not open session to host");
			update_per_host_status(GPDB_SNMP_STATUS_UNREACHABLE);
			last_item_in_subtree = 1;
		}
		else
		{
			char oidstring[GPMMON_SNMP_OID_STRING_BUF_SIZE] = { 0 };
			snprint_objid(oidstring, GPMMON_SNMP_OID_STRING_BUF_SIZE, nextOID, nextOID_len);
			gpmon_warningx(FLINE, 0, "snmp_array_collection got unknown error looking for OID %s at host %d\n", oidstring, hostname);
			last_item_in_subtree = 1;
		}
	
		if (response)
	    	snmp_free_pdu(response);

		if (last_item_in_subtree)
			break;
	}

	if (retVal == APR_ETIMEDOUT)
	{
		return APR_ETIMEDOUT;
	}
	return APR_SUCCESS;
}

FILE* open_snmp_file_for_host(const char* hostname)
{
	FILE* fd;
	char filename[PATH_MAX] = { 0 };
	snprintf(filename, PATH_MAX, "%s/_snmp.host.%s.txt", si.outputdir, hostname);
	fd = fopen(filename, "w");
	if (!fd)
	{
		gpmon_warningx(FLINE, 0, "could not open snmp data file: %s", filename);
	}
	return fd;
}

void close_snmp_file_for_host(FILE* fd, const char* hostname)
{
	char filename[PATH_MAX] = { 0 };
	char filename_tmp[PATH_MAX] = { 0 };

	fclose(fd);

	snprintf(filename,     PATH_MAX, "%s/snmp.host.%s.txt",  si.outputdir, hostname);
	snprintf(filename_tmp, PATH_MAX, "%s/_snmp.host.%s.txt", si.outputdir, hostname);

	rename(filename_tmp, filename);
}

void run_snmp_report_for_dell_host(const char* categoryname, const char* hostname)
{
	FILE* fd = open_snmp_file_for_host(hostname);
	if (!fd)
		return;

	init_per_host_status();

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.300.10.1.8", "System Manufacturer", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.300.10.1.9", "System Model", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.400.10.1.6", "OS Name", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.400.10.1.7", "OS Version", gpmon_format_snmp_string, FALSE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.600.12.1.5", "Power Supply Status", gpmon_format_dell_status, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.600.50.1.5", "Battery Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.700.12.1.8", "Cooling Device Name", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.700.12.1.5", "Cooling Device Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.30.1.16", "Processor Version", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.30.1.5", "Processor Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.40.1.13", "Cache Device Size", gpmon_format_snmp_int, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.40.1.5", "Cache Device Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.400.20.1.4", "OS Memory Status", gpmon_format_dell_status, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.50.1.5", "Memory Device Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.90.1.5", "Network Device", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.90.1.7", "Network Device Vendor", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.90.1.11", "Network Device IP", gpmon_format_snmp_ip, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.90.1.12", "Network Device Subnet Mask", gpmon_format_snmp_ip, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10892.1.1100.90.1.4", "Network Device Status", gpmon_format_dell_network_device_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.1.1.2", "Controller Name", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.1.1.3", "Controller Vendor", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.1.1.5", "Controller Status", format_dell_controller_status, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.15.1.5", "Controller Battery Status", gpmon_format_dell_status, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.2", "Virtual Disk Name", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.3", "Virtual Disk Device Name", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.19", "Virtual Disk Status", gpmon_format_dell_status, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.13", "Virtual Disk Layout", format_dell_virtual_disk_layout, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.6", "Virtual Disk MB", gpmon_format_snmp_int, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.10", "Virtual Disk Write Policy", format_dell_virtual_disk_write_policy, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.11", "Virtual Disk Read Policy", format_dell_virtual_disk_read_policy, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.140.1.1.4", "Virtual Disk State", format_dell_virtual_disk_state, TRUE)) goto BAIL;

	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.4.1.2", "Array Disk Name", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.4.1.3", "Array Disk Vendor", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.674.10893.1.20.130.4.1.23", "Array Disk Status", gpmon_format_dell_status, TRUE)) goto BAIL;

BAIL:
	close_snmp_file_for_host(fd, hostname);
	write_line_in_hostlist_file(categoryname, hostname);
}

void run_snmp_report_for_brocade_switch(const char* categoryname, const char* hostname)
{
	FILE* fd = open_snmp_file_for_host(hostname);
	if (!fd)
		return;

	init_per_host_status();

	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.1.5", "Host Name", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.1.1", "Description", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.1588.2.1.1.1.1.7", "Operational Status", format_brocade_switch_status, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.4.1.1588.2.1.1.1.1.6", "Firmware Version", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.2.2.1.2", "Interface Description", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.2.2.1.8", "Interface Status", format_network_interface_status, TRUE)) goto BAIL;

BAIL:
	close_snmp_file_for_host(fd, hostname);
	write_line_in_hostlist_file(categoryname, hostname);
}

void run_snmp_report_for_allied_telesis_switch(const char* categoryname, const char* hostname)
{
	FILE* fd = open_snmp_file_for_host(hostname);
	if (!fd)
		return;

	init_per_host_status();

	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.1.5", "Host Name", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.1.1", "Description", gpmon_format_snmp_string, FALSE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.2.2.1.2", "Interface Description", gpmon_format_snmp_string, TRUE)) goto BAIL;
	if (snmp_array_collection(fd, hostname, "1.3.6.1.2.1.2.2.1.8", "Interface Status", format_network_interface_status, TRUE)) goto BAIL;

BAIL:
	close_snmp_file_for_host(fd, hostname);
	write_line_in_hostlist_file(categoryname, hostname);
}

void write_lastreport_status()
{
	FILE* fd = fopen(si.lastreport_tmpfile, "w");
	if (!fd)
	{
		gpmon_warningx(FLINE, 0, "could not open snmp lastreport file: %s", si.lastreport_tmpfile);
		return;
	}

	fprintf(fd, "%" FMT64 "|%" FMT64 "\n", (apr_int64_t)si.lastreport_started, (apr_int64_t)si.lastreport_done);
	fclose(fd);
	rename(si.lastreport_tmpfile, si.lastreport_file);
}

void snmp_start_report()
{
	si.lastreport_started = time(NULL);
	write_lastreport_status();
}

void snmp_done_report()
{
	si.lastreport_done = time(NULL);
	write_lastreport_status();
}

/* seperate thread for snmp */
void snmp_report(host_t* tab, int tabsz)
{
	int i;
	struct SnmpConfigEntry* iter;

	if (!si.snmpInitialized)
		return; // there was a failure in the setup phase ... do not collect data

	// memset the summary data
	// open the summary per history file

	snmp_start_report();

	if (!snmp_open_hostlist_file())
	{
		gpmon_warningx(FLINE, 0, "could not open snmp hostlist output file(%s) not updating snmp info\n", si.hostlist_file);
		return;
	}

	// call snmp hosts from config file
	iter = si.snmpConfigHead;
	while (iter)
	{
		iter->function(iter->category, iter->hostname);
		iter = iter->next;
	}

	// call snmp hosts from gp_segment_configuration
	for (i = 0; i < tabsz; i++)
    {
		if (tab[i].is_master)
		{
			run_snmp_report_for_dell_host("Master Server", tab[i].hostname);
		}
		else
		{
			run_snmp_report_for_dell_host("Segment Server", tab[i].hostname);
		}
	}

	snmp_close_hostlist_file();

	snmp_done_report();
}

// return 0 for success 1 for failure
int find_token_in_config_string(char* buffer, char**result, const char* token)
{
	char lookfor[128];
	char* end;

	snprintf(lookfor, 128, "%s=\'", token);
	*result = strstr(buffer, lookfor);

	if (!*result)
		return 1;
	
	// within string find first single quote
	*result = strchr(*result, '\'');
	if (!result)
		return 1;

	// advance beyond first single quote
	(*result)++;

	if (!*result)
		return 1;

	// within string find second single quote
	end = strchr(*result, '\'');
	if (!end)
		return 1;

	// set second single quote to null
	*end = '\0';
	return 0;
}

void parse_snmp_config_line(char* line)
{
	// a line starting with a # is a comment
	// Within a line we look for following tokens in order:
	// 		Category='FOO'
	// 		Device='FOO'
	// 		Host='FOO'
	// Here is an exmaple:
	//Category='Admin Switch'         Device='AlliedTelesis'   Host='a-sw-1-app1'
	//Category='Interconnect Switch'  Device='Brocade8000'     Host='i-sw-1-app1'
	//Category='Interconnect Switch'  Device='Brocade8000'     Host='i-sw-2-app1'
	char* host;
	char* device;
	char* category;

	if (!line)
		return;

	if (line[0] == '#')
		return;

	// we do these in reverse order so inserting null chars does not prevent finding other tokens
	if (find_token_in_config_string(line, &host, "Host"))
		return;

	if (find_token_in_config_string(line, &device, "Device"))
		return;

	if (find_token_in_config_string(line, &category, "Category"))
		return;

	gpdb_snmp_device_function* fPtr = NULL;

	if (strcmp(device, "Brocade8000") == 0)
	{
		fPtr = run_snmp_report_for_brocade_switch;
	}
	else if (strcmp(device, "AlliedTelesis") == 0)
	{
		fPtr = run_snmp_report_for_allied_telesis_switch;
	}

	if (!fPtr)
	{
		gpmon_warningx(FLINE, 0, "snmp config line has unrecognized device.  CATEGORY='%s' DEVICE='%s' HOST='%s'\n", category, device, host);
		return;
	}

	addSnmpConfigEntry(category, host, fPtr);
}

void read_snmp_config(const char* configfile)
{
    char buffer[1024] = { 0 };
    char *line = NULL;
    FILE *fp = NULL;
	struct SnmpConfigEntry* iter;

	if (!configfile)
	{
		gpmon_warningx(FLINE, 0, "no snmp config file.  Only report SNMP data for segment and master servers.");
		return;
	}

    fp = fopen(configfile, "r");
    if (!fp)
    {
		gpmon_warningx(FLINE, 0, "could not open snmp config file: %s", configfile);
        return;
    }

    while (NULL != fgets(buffer, 1024, fp))
    {
        // remove new line 
        line = gpmon_trim(buffer);
		parse_snmp_config_line(line);
	}

	fclose(fp);

	iter = si.snmpConfigHead;
	while (iter)
	{
		TR0(("SNMP COLLECTION CATEGORY: '%s' HOST: '%s' func: '%p'\n", iter->category, iter->hostname, iter->function));
		iter = iter->next;
	}
}

void read_state_from_last_report_file()
{
    char buffer[1024] = { 0 };
	FILE* fd = fopen(si.lastreport_file, "r");
	char* p;
	if (!fd)
		return; // can't open file, no state to obtain
	
    while (NULL != fgets(buffer, 1024, fd))
    {
		p = strchr(buffer, '|');
		if (!p)
			return; // did not find expected delimiter

		p++;

		si.lastreport_done = atoi(p);

		break; // only read one line
	}

	fclose(fd);
}

void gpmmon_init_snmp(const char* configfile, const char* outdir)
{
	read_snmp_config(configfile);

	snprintf(si.outputdir, PATH_MAX, "%s", outdir);
	snprintf(si.lastreport_file,    PATH_MAX, "%s/lastreport.txt", outdir);
	snprintf(si.lastreport_tmpfile, PATH_MAX, "%s/_lastreport.txt", outdir);
	snprintf(si.hostlist_file,  	PATH_MAX, "%s/hostlistreport.txt", outdir);
	snprintf(si.hostlist_tmpfile, 	PATH_MAX, "%s/_hostlistreport.txt", outdir);

	if (gpmon_recursive_mkdir(si.outputdir))
	{
		gpmon_warningx(FLINE, 0, "could not make the output directory for snmp data: %s", si.outputdir);
		return;
	}
	
	read_state_from_last_report_file();

	init_snmp("gpmmon");
	snmp_out_toggle_options("n"); // make output of oids numeric only

	si.snmpInitialized=true;
}
