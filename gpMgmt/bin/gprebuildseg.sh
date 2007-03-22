#!/bin/bash
#	Filename:-		gprebuildseg.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released.
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Rebuilds segments from a rebuilt master segment instance
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#
#******************************************************************************
# Prep Code
# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
WORKDIR=`dirname $0`
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
if [ -f $FUNCTIONS ]; then
		. $FUNCTIONS
else
		echo "[FATAL:]-Cannot source $FUNCTIONS file Script Exits!"
		exit 2
fi
#******************************************************************************
# Location Variables
#******************************************************************************

#******************************************************************************
# Command Variables
#******************************************************************************

#******************************************************************************
# Script Specific Variables
#******************************************************************************
INTERACTIVE=1
VERBOSE=1
WAIT="-w"
MODE=smart
TIME=`$DATE +%H%M%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
unset START_gpsync
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
	if [ -f ${MPPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
		$MORE ${MPPDOCDIR}/$HELP_DOC_NAME
		exit 0
	else
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Builds segment instances from information contained in a Master"
		$ECHO "      instance that was built via use of gprebuildcluster.sh with -m"
		$ECHO "      option. This allows original segment instance hostnames to be "
		$ECHO "      changed following the Master instance rebuild"
		$ECHO "      Script expects the master instance to be shutdown, if master active"
		$ECHO "      use gpstop.sh -m to close down"
		$ECHO
		$ECHO "      Usage:"
		$ECHO "      $0 [OPTIONS]"
		$ECHO
		$ECHO "      General options:"
		$ECHO "      -?, display this help message & exit" 
		$ECHO "      -v, display version information & exit"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "      -a, don't ask to confirm re-build [default:- ask]"
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO
		$ECHO "      Segment rebuild options:"
		$ECHO "      -d, <master data directory name> directory where a master database is located"
		$ECHO "         [mandatory]"
		$ECHO "      -n, <locale setting> locale setting when database initialized [dafeult:-$DEFAULT_LOCALE_SETTING]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but master instance operational"
		$ECHO "      2 Fatal error, database not rebuilt, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

START_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Expect the master database instance to be closed down, check for postmaster.pid file
	if [ -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
			ERROR_EXIT "[FATAL]:-Expected Master to be shutdown, use gpstop.sh -m to shutdown master instance" 2
	else
		LOG_MSG "[INFO]:-Starting Master instance in admin mode" 1
		if [ -f $GPSTART ];then
			$GPSTART -qam -d $MASTER_DATA_DIRECTORY
			if [ $? -eq 2 ];then
			ERROR_CHK $? "start Master instance in admin mode" 2
			else
			ERROR_CHK $? "start Master instance in admin mode" 1
			fi
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $GPSTART" 2
		fi		
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears Master instance is operational, shutdown with gpstop.sh -m" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of Master instance from admin mode" 1
		if [ -f $gpstop ];then
			$gpstop -qam -d $MASTER_DATA_DIRECTORY
			ERROR_CHK $? "stop Master instance from admin mode" 2
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $gpstop" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_SEGMENT_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MASTER_MPP_NAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select gpname from $GP_ID_TABLE;"`
	ERROR_CHK $? "get master instance instance name" 2
	REBUILD_ARRAY=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select hostname, port, datadir, dbid, content, definedprimary from $CONFIG_TABLE where content<>-1;"`)
	ERROR_CHK $? "get instance segment instance details " 2
	MASTER_IP_ADDRESS_ALL=(`$IFCONFIG |$GREP "inet addr"|$GREP -v addr:127|$AWK '{print $2}'|$CUT -d: -f2`)	
	ERROR_CHK $? "get master host IP details" 2
	MASTER_IP_ADDRESS=(`$ECHO ${MASTER_IP_ADDRESS_ALL[@]}|$TR ' ' '\n'|$SORT -u|$TR '\n' ' '`)
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	MASTER_TOTAL_SEG=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select numsegments from $GP_ID_TABLE;"`
	ERROR_CHK $? "get total instance segments" 2
	MASTER_HOST=`$HOSTNAME`
	STANDBY_COUNT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select count(*) from gp_configuration where content=-1 and hostname<>'$MASTER_HOST';"`
	if [ $STANDBY_COUNT -eq 1 ];then
		LOG_MSG "[INFO]:-Master standby found, will process details" 1
		STANDBY_MASTER_HOSTNAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select hostname from gp_configuration where content=-1 and hostname<>'$MASTER_HOST';"`
	else
		LOG_MSG "[INFO]:-No Master standby host found" 1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_QES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for SEG_INFO in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $SEG_INFO
		#Check hosts can be reached
		PING_HOST $SEG_HOSTNAME
		#Check that we do not already have segment directory 
		CHK_DIR $SEG_DATADIR $SEG_HOSTNAME
		if [ $EXISTS -eq 0 ];then
			ERROR_EXIT "[FATAL]:-Segment directory $SEG_DATADIR exists on $SEG_HOSTNAME" 2
		fi
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "MPP segment instance rebuild parameters" 1
	LOG_MSG "---------------------------------------" 1
	LOG_MSG "MPP Master hostname            = $MASTER_HOST" 1
	LOG_MSG "MPP Master data directory      = $MASTER_DATA_DIRECTORY" 1
	LOG_MSG "MPP Master port                = $MASTER_PORT" 1
	LOG_MSG "MPP Array name                 = $MASTER_MPP_NAME"
	if [ $STANDBY_COUNT -eq 1 ];then
		LOG_MSG "MPP Master standy hostname     = $STANDBY_MASTER_HOSTNAME" 1
	else
		LOG_MSG "MPP Build Master standby       = Off" 1
	fi
	for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
	do
		LOG_MSG "MPP Master IP addr             = $MASTER_IP" 1
		IP_ALLOW="`$ECHO $MASTER_IP|$CUT -d. -f1-3`.0/24"
		LOG_MSG "MPP Master IP allow            = $IP_ALLOW" 1
	done
	LOG_MSG "MPP Segment instance rebuild list" 1
	LOG_MSG "---------------------------------" 1
	for I in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $I
		if [ "$SEG_DEFINED" == "t" ]; then
			SEG_DEFINED="Primary"
		else
			SEG_DEFINED="Mirror"
		fi
		LOG_MSG "[INFO]:-$SEG_HOSTNAME $SEG_DATADIR $SEG_PORT  $SEG_DBID $SEG_DEFINED" 1
	done
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP rebuild Yy/Nn>"
		read REPLY
		if [ -z $REPLY ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


SET_VAR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	SEG_HOSTNAME=`$ECHO $1|$CUT -d"|" -f1`
	SEG_PORT=`$ECHO $1|$CUT -d"|" -f2`
	SEG_DATADIR=`$ECHO $1|$CUT -d"|" -f3`
	SEG_DBID=`$ECHO $1|$CUT -d"|" -f4`
	SEG_CONTENT=`$ECHO $1|$CUT -d"|" -f5`
	SEG_DEFINED=`$ECHO $1|$CUT -d"|" -f6`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

BUILD_SEGMENT_INSTANCES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Creating segment instance databases" 1
	for SEG_INFO in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $SEG_INFO
		LOG_MSG "[INFO]:-Processing segment host $SEG_HOSTNAME port $SEG_PORT data directory $SEG_DATADIR dbid $SEG_DBID" 1
		RUN_COMMAND_REMOTE ${SEG_HOSTNAME} "${EXPORT_LIB_PATH};$INITDB -E $ENCODING --locale=$LOCALE_SETTING -D $SEG_DATADIR"
		BACKOUT_COMMAND "$TRUSTED_SHELL ${SEG_HOSTNAME} \"$RM -rf $SEG_DATADIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $SEG_DATADIR on $SEG_HOSTNAME\""
		LOG_MSG "[INFO]:-Configuring segment $PG_CONF" 1
		ED_FILE ${SEG_DATADIR}/$PG_CONF "#port" port=$SEG_PORT 0 $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF listen_addresses listen_addresses=\'*\' 0  $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF checkpoint_segments checkpoint_segments=$CHECK_POINT_SEGMENTS 0 $SEG_HOSTNAME
		LOG_MSG "[INFO]:-Configuring segment $PG_HBA" 1
		for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"		
		do
			RUN_COMMAND_REMOTE ${SEG_HOSTNAME} "$ECHO host  all     all     ${MASTER_IP}/32 trust >> ${SEG_DATADIR}/$PG_HBA"
		done
		LOG_MSG "[INFO]:-Starting Functioning instance on segment ${SEG_HOSTNAME}" 1
		$TRUSTED_SHELL ${SEG_HOSTNAME} "${EXPORT_LIB_PATH};export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" start" 
		if [ $? -ne 0 ]; then
			BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -f ${SEG_DATADIR}.log\""
			BACKOUT_COMMAND "$ECHO \"Removing segment log file ${SEG_DATADIR}.log on segment $SEG_HOSTNAME\""
			BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -Rf ${SEG_DATADIR}\""
			BACKOUT_COMMAND "$ECHO \"Removing segment data directory $SEG_DATADIR\""
			$TRUSTED_SHELL ${SEG_HOSTNAME} "$CAT ${SEG_DATADIR}.log "|$TEE -a $LOG_FILE
			ERROR_EXIT "[FATAL]:-Failed to start segment instance database" 2
		fi
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -f ${SEG_DATADIR}.log\""
		BACKOUT_COMMAND "$ECHO \"Removing segment log file ${SEG_DATADIR}.log on segment $SEG_HOSTNAME\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -Rf ${SEG_DATADIR}\""
		BACKOUT_COMMAND "$ECHO \"Removing segment data directory $SEG_DATADIR\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"${EXPORT_LIB_PATH};export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" -m immediate  stop\""
		BACKOUT_COMMAND "$ECHO \"Stopping segment instance\""
		LOG_MSG "[INFO]:-Successfully started segment instance on $SEG_HOSTNAME" 1
		LOG_MSG "[INFO]:-Updating segment $GP_ID_TABLE system table" 1
		RUN_COMMAND_REMOTE $SEG_HOSTNAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $SEG_PORT $DEFAULTDB -c \"insert into $GP_ID_TABLE values ('${MASTER_MPP_NAME}',${MASTER_TOTAL_SEG},${SEG_DBID},${SEG_CONTENT});\"" 
		ERROR_CHK $? "update $SEG_HOSTNAME $GP_ID_TABLE record entry with DBID value ${SEG_DBID}" 2
		LOG_MSG "[INFO]:-Successfully updated $SEG_HOSTNAME $GP_ID_TABLE system table"
		LOG_MSG "[INFO]:-Commencing shutdown of segment instance on $SEG_HOSTNAME" 1
		$TRUSTED_SHELL ${SEG_HOSTNAME} "${EXPORT_LIB_PATH};export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" stop" 
		ERROR_CHK $? "shutdown of instance on port $SEG_PORT data directory $SEG_DATADIR host $SEG_HOSTNAME" 2
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_ARRAY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CHK_FILE $GPSTART
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $GPSTART to start MPP instance" 2
	else
		LOG_MSG "[INFO]:-Starting the MPP instance" 1
		$GPSTART -d $MASTER_DATA_DIRECTORY -a
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-The MPP instance has now been successfully rebuilt and is now operational" 1
	LOG_MSG "[INFO]:-Rebuild details" 1
	LOG_MSG "[INFO]:-Master database host       = $MASTER_HOST" 1
	LOG_MSG "[INFO]:-Master data directory      = $MASTER_DATA_DIRECTORY" 1
	LOG_MSG "[INFO]:-Master database port       = $MASTER_PORT" 1
	if [ $STANDBY_COUNT -eq 1 ];then
	LOG_MSG "[INFO]:-Master standby host        = $MASTER_STANDBY_HOSTNAME" 1
	fi
	LOG_MSG "[INFO]:-To remove this instance use gpdeletecluster.sh" 1
	LOG_MSG "[INFO]:-The databases can now be restored using gp_restore" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":vt'?'aip:qn:omc:s:l:d:!D" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			! ) SCRIPT_USAGE=1
			    USAGE ;;
			d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			n ) LOCALE_SETTING=$OPTARG ;;
			D ) DEBUG_LEVEL=1 ;;
			* ) USAGE ;;
		esac
	done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
		EXIT_STATUS=1
		BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
		BACKOUT_FILE=$LOG_DIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
	fi
else
	BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
fi
if [ x"" == x"$MASTER_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply master data directory location with -d option" 2
fi
LOG_MSG "[INFO]:-Start Main"
if [ x"" == x"$LOCALE_SETTING" ];then
        LOCALE_SETTING=$DEFAULT_LOCALE_SETTING
        LOG_MSG "[INFO]:-Set locale to default value $LOCALE_SETTING" 1
fi
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
START_MASTER_DB
GET_QD_DB_NAME
GET_SEGMENT_INFO
CHK_QES
STOP_MASTER_DB
DISPLAY_PARAM
BUILD_SEGMENT_INSTANCES
START_ARRAY
if [ $STANDBY_COUNT -eq 1 ];then
	$GPINITSTANDBY -s $STANDBY_MASTER_HOSTNAME -n -a
fi
DISPLAY_INFO
LOG_MSG "[INFO]:-End Main"
$RM -f $BACKOUT_FILE
exit $EXIT_STATUS
