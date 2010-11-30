#include "postgres.h"
#include "c.h"
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include "emcconnect/api.h"

// NOTE: code in this file is shared between GPDB and gpperfmon processes.
//  	 The code is bare bones C as much as possible to prevent having to link additional GPDB code in gpperfmon

int gpmon_emcconnect_create_xml(char* buffer, size_t bufsize, EmcConnectSymptom symptom, EmcConnectStatus status, EmcConnectSeverity severity, 
								char* gpdbVersion, char* serialNumber, char* siteName, char* hostname,  char* msg);

int gpmon_emcconnect_create_gpmon_string(char* buffer, size_t bufsize, EmcConnectSymptom symptom, EmcConnectDetailedSymptom dSymptom,
					EmcConnectStatus status, EmcConnectSeverity severity, int historyOnly, char* displayname,  
					char* snmpoid,  char* hostname,  char* msg, int yr, int mn, int dy, int hr, int min, int sec);

const char* gpmon_emcconnect_print_status(EmcConnectStatus status)
{
	switch(status)
	{
		case EMC_CONNECT_STATUS_UNKNOWN:
			return "Unknown";
		break;
		case EMC_CONNECT_STATUS_OK:
			return "OK";
		break;
		case EMC_CONNECT_STATUS_WARNING:
			return "Warning";
		break;
		case EMC_CONNECT_STATUS_FAILED:
			return "Failed";
		break;
		default:
			return "other";
		break;
	}
}

const char* gpmon_emcconnect_print_severity(EmcConnectSeverity severity)
{
	switch(severity)
	{
		case EMC_CONNECT_SEVERITY_UNKNOWN:
			return "Unknown";
		break;
		case EMC_CONNECT_SEVERITY_ERROR:
			return "Error";
		break;
		case EMC_CONNECT_SEVERITY_WARNING:
			return "Warning";
		break;
		case EMC_CONNECT_SEVERITY_INFO:
			return "Info";
		break;
		case EMC_CONNECT_SEVERITY_DEBUG:
			return "Debug";
		break;
		default:
			return "other";
		break;
	}
}

const char* emcconnect_alert_template = \
"<?xml version=\"1.0\" encoding=\"UTF-8\"?> \n\
<ConnectHome SchemaVersion=\"1.0.8\" \n\
    xsi:schemaLocation=\"/opt/connectemc/schema/ema_ConnectHome108.xsd\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"> \n\
    <TransType>10</TransType> \n\
    <TransTypeDesc>EventXML</TransTypeDesc> \n\
    <TransID>0</TransID> \n\
    <Node ID=\"%s\" State=\"Online\" Status=\"OK\"> \n\
        <Identifier> \n\
            <ClarifyID>%s</ClarifyID> \n\
            <SiteName>%s</SiteName> \n\
            <Vendor>EMC</Vendor> \n\
            <DeviceType>DCA1</DeviceType> \n\
            <Model></Model> \n\
            <SerialNumber>%s</SerialNumber> \n\
            <WWN></WWN> \n\
            <Platform></Platform> \n\
            <OS>Linux</OS> \n\
            <OS_VER></OS_VER> \n\
            <UcodeVer>%s</UcodeVer> \n\
            <EmbedLevel>1</EmbedLevel> \n\
            <InternalMaxSize>0</InternalMaxSize> \n\
            <Comment></Comment> \n\
        </Identifier> \n\
        <Connection> \n\
            <ConnectType></ConnectType> \n\
            <AccessType></AccessType> \n\
            <Version></Version> \n\
            <InPolicy></InPolicy> \n\
            <OutPolicy></OutPolicy> \n\
            <RouterIP></RouterIP> \n\
            <IPAddress></IPAddress> \n\
            <IPName></IPName> \n\
            <NAT_IP></NAT_IP> \n\
            <EMC_IP></EMC_IP> \n\
            <State></State> \n\
            <UserID></UserID> \n\
            <AppName></AppName> \n\
            <UdpSocket></UdpSocket> \n\
            <ConnectNum></ConnectNum> \n\
            <Port></Port> \n\
        </Connection> \n\
        <HeartBeat/> \n\
        <InternalData> \n\
            <FileList/> \n\
            <EventList> \n\
                <Event> \n\
                    <SymptomCode>%d</SymptomCode> \n\
                    <Category>Status</Category> \n\
                    <Severity>%s</Severity> \n\
                    <Status>%s</Status> \n\
                    <Component>%s</Component> \n\
                    <EventData></EventData> \n\
                    <Description>%s</Description> \n\
                </Event> \n\
            </EventList> \n\
        </InternalData> \n\
        <ExternalFiles/> \n\
    </Node> \n\
</ConnectHome>";


// return 0 for success
int gpmon_emcconnect_create_xml(char* buffer, 
										size_t bufsize, 
										EmcConnectSymptom symptom,
										EmcConnectStatus status,
										EmcConnectSeverity severity,
										char* gpdbVersion,
										char* serialNumber, 
										char* siteName, 
										char* hostname,  
										char* msg)
{
	int retVal = snprintf(buffer, bufsize, emcconnect_alert_template, serialNumber, serialNumber, siteName, serialNumber, gpdbVersion,
							(int)symptom, gpmon_emcconnect_print_severity(severity), gpmon_emcconnect_print_status(status), 
							hostname, msg);

	if (retVal <= 0)
		return 1;
	else if (retVal >= bufsize)
		return 1;
	else
		return 0;
}


const char* emcconnect_alert_gpmon_template = "%d-%02d-%02d %02d:%02d:%02d|%s|%d|%d|%s|%s|%s|%s|%s|%s\n";

// return 0 for success
int gpmon_emcconnect_create_gpmon_string(char* buffer, 
										size_t bufsize, 
										EmcConnectSymptom symptom,
										EmcConnectDetailedSymptom dSymptom,
										EmcConnectStatus status,
										EmcConnectSeverity severity,
										int historyOnly,
										char* displayname,  
										char* snmpoid,  
										char* hostname,  
										char* msg,
										int yr,
										int mn,
										int dy,
										int hr,
										int min,
										int sec)
{
	int i;
	int retVal = snprintf(buffer, bufsize, emcconnect_alert_gpmon_template, 
						yr, mn, dy, hr, min, sec,
						hostname, 
						(int)symptom, 
						(int)dSymptom, 
						displayname, 
						snmpoid, 
						gpmon_emcconnect_print_severity(severity), 
						gpmon_emcconnect_print_status(status), 
						historyOnly ? "f" : "t", // attempted transport
						msg);

	if (retVal <= 0)
		return 1;
	else if (retVal >= bufsize)
		return 1;


	// safety check on output string
	// should be ok for | delimited external table
	// loop to retVal -1 so we dont trim the final newline
	for (i = 0; i < retVal-1; ++i)
	{
		if (buffer[i] == 0)
		{
			break; // should never get here ... but check to be paranoid
		}

		if (buffer[i] == 10 || buffer[i] == 13) 
		{
			buffer[i] =  32; // replace newline with space
		}
	}

	return 0;
}


#define GPMON_EMCCONNECT_SERIALNUM_LEN 32
#define GPMON_EMCCONNECT_SERIALNUMBER "/opt/greenplum/serialnumber"
#define GPMON_EMCCONNECT_POLL_DIR "/opt/greenplum/connectemc/poll"
#define GPMON_EMCCONNECT_BUFFER_SIZE 65536
#define GPMON_EMCCONNECT_FILENAME_MAX 256

#define GPMON_EMCCONNECT_DOS_INTERVAL 900            // every 15 minutes reset the DOS counter
#define GPMON_EMCCONNECT_DOS_MAX_COUNTER 1000        // do not send more than the DOS counter per DOS interval

// return 0 for success
// not a thread safe function
int gpmon_emcconnect_send_alert(EmcConnectSymptom symptom, EmcConnectDetailedSymptom dSymptom,
								EmcConnectStatus status,
								EmcConnectSeverity severity,
								EmcConnectOrigin origin,
								int historyOnly,
								char* displayname,  
								char* snmpoid,  
								char* hostname,  
								char* msg)
{
	static char* buffer = NULL;
	static char serial_num[GPMON_EMCCONNECT_SERIALNUM_LEN];
	static bool serialNumberFound = false;
	static unsigned short counter = 0;

   	static unsigned int dos_counter = 1;    // how many alerts have been sent since the beginning of the DOS interval
	static time_t last_dos_check = 0;       // timestamp of when the DOS counter was reset

	char filename_buf[PATH_MAX];
	char tmp_filename_buf[PATH_MAX];

	FILE* fd = NULL;
	int retVal;
	struct tm time_result;
	time_t now;
	unsigned short mon, day, year, hr, min, sec;
	char fn_buffer[GPMON_EMCCONNECT_FILENAME_MAX];

	if (!buffer)
	{
		buffer = malloc(GPMON_EMCCONNECT_BUFFER_SIZE);
		if (!buffer)
			return 1;
	}

	if (!serialNumberFound)
	{
		FILE* serial_fd;
		char* newline;

		serial_fd = fopen(GPMON_EMCCONNECT_SERIALNUMBER, "r");
		if (!serial_fd)
			return 2;

		if (!fgets(serial_num, GPMON_EMCCONNECT_SERIALNUM_LEN, serial_fd))
			return 3;

		fclose(serial_fd);

		newline = strchr(serial_num, '\n');
		if (newline)
			*newline = 0;

		serialNumberFound = true;
	}

	now = time(NULL);

	// it has been more than enough time to reset the DOS counter
	if (now > (last_dos_check + GPMON_EMCCONNECT_DOS_INTERVAL))
	{
		last_dos_check = now;
		dos_counter = 0;
	}
	else if (dos_counter >= GPMON_EMCCONNECT_DOS_MAX_COUNTER)
	{
		return 4; // Denial Of Service prevention.... don't send more alerts in this time period
	}

	dos_counter++;

	if (!localtime_r(&now, &time_result))
		return 5;

	mon = time_result.tm_mon + 1;
	day = time_result.tm_mday;
	year = time_result.tm_year - 100;
	hr = time_result.tm_hour;
	min = time_result.tm_min;
	sec = time_result.tm_sec;

	// ENCODING OF FILENAME
	// NOTE: we have origin field to distinguish between gpperfmon and GPDB alerting as the file creator
	// NOTE: we have 4 digits to represent a unique filenumber from this origin for this minute
	retVal = snprintf(fn_buffer, GPMON_EMCCONNECT_FILENAME_MAX, "%s_%02d%02d%02d_%02d%02d%01d%04d", serial_num, mon, day, year, hr, min, origin, ++counter%10000);
	if (retVal < 0)
		return 6;
	if (retVal >= GPMON_EMCCONNECT_FILENAME_MAX)
		return 7;

	retVal = snprintf(filename_buf, PATH_MAX, "%s/%s_%s.txt", GPMON_EMCCONNECT_POLL_DIR, "GPMON", fn_buffer);
	if (retVal < 0)
		return 8;
	if (retVal >= PATH_MAX)
		return 9;

	retVal = snprintf(tmp_filename_buf, PATH_MAX, "%s/%s_%s.txt", GPMON_EMCCONNECT_POLL_DIR, "TMP_GPMON", fn_buffer);
	if (retVal < 0)
		return 10;
	if (retVal >= PATH_MAX)
		return 11;

	retVal = gpmon_emcconnect_create_gpmon_string(buffer, GPMON_EMCCONNECT_BUFFER_SIZE, symptom, dSymptom, status, severity, 
							historyOnly, displayname, snmpoid, hostname, msg,
							year+2000, mon, day, hr, min, sec);
	if (retVal)
		return 12;

	fd = fopen(tmp_filename_buf, "w");
	if (!fd)
		return 13;

	fprintf(fd, "%s", buffer);

	fclose(fd);

	retVal = rename(tmp_filename_buf, filename_buf);
	if (retVal)
		return 14;

	if (historyOnly)
		return 0;

	retVal = gpmon_emcconnect_create_xml(buffer, GPMON_EMCCONNECT_BUFFER_SIZE, symptom, status, severity, GP_VERSION, serial_num, "", hostname, msg);
	if (retVal)
		return 15;

	retVal = snprintf(filename_buf, PATH_MAX, "%s/%s_%s.xml", GPMON_EMCCONNECT_POLL_DIR, "RSC", fn_buffer);
	if (retVal < 0)
		return 16;
	if (retVal >= PATH_MAX)
		return 17;

	retVal = snprintf(tmp_filename_buf, PATH_MAX, "%s/%s_%s.tmp", GPMON_EMCCONNECT_POLL_DIR, "TMP", fn_buffer);
	if (retVal < 0)
		return 11;
	if (retVal >= PATH_MAX)
		return 18;

	fd = fopen(tmp_filename_buf, "w");

	if (!fd)
		return 19;

	fprintf(fd, "%s", buffer);

	fclose(fd);

	retVal = rename(tmp_filename_buf, filename_buf);
	if (retVal)
		return 20;

	return 0;
}

