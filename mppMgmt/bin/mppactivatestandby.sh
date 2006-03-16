#!/bin/bash
#	Filename:-		mppactivatestandby.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Not yet complete, 1st phase only.
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Starts the cluster database.
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
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
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
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
FORCE_ACTIVATE=0
unset START_MPPSYNC
START_PROG=$MPPSTART
STOP_PROG=$MPPSTOP
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Activates a standby QD database instance when the primary QD instance has"
		$ECHO "      failed. Will stop the mppsyncprocess, update the system catalog tables and"
		$ECHO "      start the cluster with the standby QD as the new master."
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
		$ECHO "      -a, don't ask to confirm standby QD activation [default:- ask]"
		$ECHO
		$ECHO "      Standby activation options:"
		$ECHO "      -d, <standby MASTER_DATA_DIRECTORY> [mandatory]"
		$ECHO "      -f, force activation if mppsyncmaster process not running [optional]"
		$ECHO "          Only use this option if it is known that the mppsynmaster process is not"
		$ECHO "          running AND the standby QD is upto date AND consistent"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but standby QD master database operational"
		$ECHO "      2 Fatal error, standby master QD database not operational, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

CHECK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check to ensure that we do not have a clash with a current setting of MASTER_DATA_DIRECTORY
	if [ x"" != x"$MASTER_DATA_DIRECTORY" ];then
		LOG_MSG "[INFO]:-MASTER_DATA_DIRECTORY value set, checking for conflict"
		if [ $MASTER_STANDBY_DATA_DIRECTORY != $MASTER_DATA_DIRECTORY ];then
			ERROR_EXIT "[FATAL]:-Current setting of MASTER_DATA_DIRECTORY not same as -d parameter" 2
		fi
	fi
	#Check that we are not trying to create a standby instance on this host
	if [ "$MASTER_STANDBY_HOSTNAME" != "`$HOSTNAME`" ];then
		ERROR_EXIT "[FATAL]:-Must run this script on the standby master host" 2
	fi
	#Check that the proposed master standby directory does not already exist
	CHK_DIR $MASTER_STANDBY_DATA_DIRECTORY
	if [ $EXISTS -eq 1 ];then
		ERROR_EXIT "[FATAL]:-Directory $MASTER_STANDBY_DATA_DIRECTORY does not exist on $MASTER_STANDBY_HOSTNAME" 2
	fi
	#Check to see if the mppsyncmaster process is active
	CHK_FILE $MASTER_STANDBY_DATA_DIRECTORY/$PG_PID
	if [ $EXISTS -ne 0 ];then
		if [ $FORCE_ACTIVATE -eq 1 ];then
			LOG_MSG "[WARN]:-Forcing standby QD activation, even though mppsyncmaster process not running"
		else
			ERROR_EXIT "[FATAL]:-No mppsyncmaster process running on $MASTER_STANDBY_HOSTNAME" 2
		fi
	fi	
	if [ ! -f $START_PROG ];then ERROR_EXIT "[FATAL $START_PROG does not exist" 2;fi
	if [ ! -f $STOP_PROG ];then ERROR_EXIT "[FATAL $STOP_PROG does not exist" 2;fi
	#Obtain the standby port number
	MASTER_STANDBY_PORT=`$GREP "^port=" $MASTER_STANDBY_DATA_DIRECTORY/$PG_CONF|$CUT -d'=' -f2|$AWK '{print $1}'`
	ERROR_CHK $? "obtain standby master port number" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MPPSYNC_PROCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	STOP_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY
	if [ $RETVAL -ne 0 ] && [ $FORCE_ACTIVE -eq 0 ] ;then
		ERROR_EXIT "[FATAL]:-Failed to shutdown mppsyncmaster process" 2;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

ADMIN_SHUTDOWN () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Stopping standby QD instance"
	$STOP_PROG -m -d $MASTER_STANDBY_DATA_DIRECTORY
	ERROR_CHK $? "shutdown standby QD instance" 1
	if [ x"" != x"$1" ];then
		LOG_MSG "[INFO]:-Re-starting sync process"
		$MPPSYNCMASTER -D $MASTER_STANDBY_DATA_DIRECTORY -p $MASTER_STANDBY_PORT -S
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_CATALOG_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Starting standby QD instance in admin mode"
	$START_PROG -m -d $MASTER_STANDBY_DATA_DIRECTORY
	ERROR_CHK $? "start standby QD in admin mode" 2
	MASTER_DBID=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select min(dbid) from mpp_configuration where content=-1;"`
	MASTER_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select hostname from mpp_configuration where dbid in ($MASTER_DBID)"`
	ERROR_CHK $? "obtain master hostname" 1
	if [ $RETVAL -ne 0 ];then 
		ADMIN_SHUTDOWN 1
		ERROR_EXIT "[FATAL]:-Unable to obtain master QD hostname" 2;fi
	MASTER_PORT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select port from mpp_configuration where dbid in ($MASTER_DBID);"`
	ERROR_CHK $? "obtain master QD port number" 1
	if [ $RETVAL -ne 0 ];then 
		ADMIN_SHUTDOWN 1
		ERROR_EXIT "[FATAL]:-Unable to obtain master QD port" 2;fi
	PING_HOST $MASTER_HOSTNAME
	if [ $RETVAL -eq 0 ];then
		LOG_MSG "[INFO]:-Master QD host $MASTER_HOSTNAME can be contacted, making further checks"
		GET_PG_PID_ACTIVE $MASTER_PORT $MASTER_HOSTNAME
		if [ $PID -ne 0 ];then
			LOG_MSG "[FATAL]:-Appears that there is an active postmaster on $MASTER_HOST port=$MASTER_PORT"
			ADMIN_SHUTDOWN 1
			ERROR_EXIT "[FATAL]:-Unable to continue, master instance still appears to be active" 2
		fi
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Master standby: $MASTER_STANDBY_HOSTNAME"
	MASTER_DBID=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select min(dbid) from mpp_configuration where content=-1;"`
	LOG_MSG "[INFO}:-Obtained MASTER_DBID as $MASTER_DBID"
###	env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT -A -t -c"update mpp_configuration set hostname='$MASTER_STANDBY_HOSTNAME' where dbid in ($MASTER_DBID);"
	env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"update mpp_configuration set hostname='$MASTER_STANDBY_HOSTNAME' where dbid in ($MASTER_DBID);"
	ERROR_CHK $? "update mpp_configuration hostname value" 2
	if [ $RETVAL -ne 0 ];then
	    ADMIN_SHUTDOWN
	    ERROR_EXIT "[FATAL]:-Unable to contine, unable to update mpp_configuration catalog" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_STANDBY_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears standby instance shutdown during this process, check standby instance log file" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of standby instance, including a call to shutdown segment instances"
		LOG_MSG "[INFO]:-Ignore any warning messages if mppstop.sh reports problems with shutdown of segment instances"
		$STOP_PROG -qfa -d $MASTER_STANDBY_DATA_DIRECTORY
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "MPP standby QD activation parameters"
	LOG_MSG "------------------------------------"
	LOG_MSG "MPP Master standby hostname       = $MASTER_STANDBY_HOSTNAME"
	LOG_MSG "MPP Master standby port           = $MASTER_STANDBY_PORT"
	LOG_MSG "MPP Master standby data directory = $MASTER_STANDBY_DATA_DIRECTORY"
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP Master standby activation Yy/Nn>"
		read REPLY
		if [ -z $REPLY ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_ARRAY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Starting the MPP array"
	$START_PROG -d $MASTER_STANDBY_DATA_DIRECTORY -a
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_INFO () {
	LOG_MSG "[INFO]:-The activation of the standby QD has completed successfully."
	LOG_MSG "[INFO]:-$MASTER_STANDBY_HOSTNAME is now the new master QD."
	LOG_MSG "[INFO]:-You will need to update your user access mechanism to reflect"
	LOG_MSG "[INFO]:-the change of master hostname."
	LOG_MSG "[INFO]:-Do not re-start the failed master QD whilst the fail-over QD is"
	LOG_MSG "[INFO]:-operational, this could result in database corruption!"
	LOG_MSG "[INFO]:-MASTER_DATA_DIRECTORY is now $MASTER_STANDBY_DATA_DIRECTORY if"
	LOG_MSG "[INFO]:-this has changed as a result of the standby QD activation, rememeber"
	LOG_MSG "[INFO]:-to change this in any startup scripts etc, that may be configured"
	LOG_MSG "[INFO]:-to set this value." 
	LOG_MSG "[INFO]:-MASTER_PORT is now $MASTER_STANDBY_PORT, if this has changed, you"
	LOG_MSG "[INFO]:-may need to make additional configuration changes to allow access"
	LOG_MSG "[INFO]:-to the MPP cluster."
	LOG_MSG "[INFO]:-When the failed master QD becomes available, run mppactivatemaster.sh"
	LOG_MSG "[INFO]:-to re-activate the master QD and return the standby QD to its previous"
	LOG_MSG "[INFO]:-state."
	LOG_MSG "[INFO]:-Statistics will need to be run against the database"
	LOG_MSG "[INFO]:-Following standby QD activation"
	LOG_MSG "[INFO]:-When convenient, run analyze against the user databases"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":v'?'aql:d:f" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			d ) MASTER_STANDBY_DATA_DIRECTORY=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			f ) FORCE_ACTIVATE=1 ;;
			* ) USAGE ;;
		esac
	done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
		EXIT_STATUS=1
		BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
MASTER_STANDBY_HOSTNAME=`$HOSTNAME`
if  [ x"" == x"$MASTER_STANDBY_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply -d <master data directory> option" 2
fi
LOG_MSG "[INFO]:-Start Main"
CHECK_PARAM
if [ x"" != x"$INTERACTIVE" ];then 
	DISPLAY_PARAM
fi
CHK_DIR $MASTER_STANDBY_DATA_DIRECTORY
if [ $EXISTS -eq 1 ];then
        ERROR_EXIT "[FATAL]:-Standby data directory $MASTER_STANDBY_DATA_DIRECTORY does not exist on this host" 2
fi
STOP_MPPSYNC_PROCESS
CHK_CATALOG_DATA
UPDATE_CATALOG
STOP_MASTER_DB
START_ARRAY
DISPLAY_INFO
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
