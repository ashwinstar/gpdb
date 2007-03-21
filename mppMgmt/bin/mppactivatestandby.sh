#!/bin/bash
#	Filename:-		mppactivatestandby.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released	
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Activates a warm standby master segment instance.
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
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
FORCE_ACTIVATE=0
unset START_MPPSYNC
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
		$ECHO "      Activates a standby master segment instance when the primary instance has"
		$ECHO "      failed. Will stop the mppsyncprocess, update the system catalog tables and"
		$ECHO "      start the instance with the standby master segment instance as the new master."
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
		$ECHO "      -a, don't ask to confirm standby master activation [default:- ask]"
		$ECHO
		$ECHO "      Standby activation options:"
		$ECHO "      -d, <standby MASTER_DATA_DIRECTORY> [mandatory]"
		$ECHO "      -f, force activation if gpsyncmaster process not running [optional]"
		$ECHO "          Only use this option if it is known that the gpsyncmaster process is not"
		$ECHO "          running AND the standby master segment is upto date AND consistent"
		$ECHO "      -c, <new standby master hostname> create a new standby master instance following"
		$ECHO "          successful activation of the standby master instance"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but standby master segment instance operational"
		$ECHO "      2 Fatal error, standby master segment instance not operational, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

CHECK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check that process running is not a postmaster
	CHK_FILE $MASTER_STANDBY_DATA_DIRECTORY/$PG_OPT
	if [ $EXISTS -eq 0 ];then
		if [ `$GREP -c postmaster $MASTER_STANDBY_DATA_DIRECTORY/$PG_OPT` -ne 0 ];then
			LOG_MSG "[FATAL]:-Have located a postmaster process on this host"
			LOG_MSG "[FATAL]:-Has the Master standby instance already been activated"
			LOG_MSG "[FATAL]:-Check output of mppstate.sh to check"
			ERROR_EXIT "[FATAL]:-Possible Standby master instance active" 2
		else
			LOG_MSG "[INFO]:-No postmaster text found in $PG_OPT"
		fi
	else
		LOG_MSG "[INFO]:-File $MASTER_STANDBY_DATA_DIRECTORY/$PG_OPT does not exist"
	fi
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
	#Check to see if the gpsyncmaster process is active
	CHK_FILE $MASTER_STANDBY_DATA_DIRECTORY/$PG_PID
	if [ $EXISTS -ne 0 ];then
		if [ $FORCE_ACTIVATE -eq 1 ];then
			LOG_MSG "[WARN]:-Forcing standby master activation, even though gpsyncmaster process not running"
		else
			ERROR_EXIT "[FATAL]:-No gpsyncmaster process running on $MASTER_STANDBY_HOSTNAME" 2
		fi
	fi	
	if [ ! -f $MPPSTART ];then ERROR_EXIT "[FATAL $MPPSTART does not exist" 2;fi
	if [ ! -f $MPPSTOP ];then ERROR_EXIT "[FATAL $MPPSTOP does not exist" 2;fi
	#Check hostname not the same as the current host if -c option supplied
	if [ x"" != x"$NEW_STANDBY_MASTER" ] && [ $NEW_STANDBY_MASTER == `$HOSTNAME` ];then
		ERROR_EXIT "[FATAL]:-New standby hostname supplied is same as current hostname"
	fi	
	#Obtain the standby port number
	MASTER_STANDBY_PORT=`$GREP "^port=" $MASTER_STANDBY_DATA_DIRECTORY/$PG_CONF|$CUT -d'=' -f2|$AWK '{print $1}'`
	ERROR_CHK $? "obtain standby master port number" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MPPSYNC_PROCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	STOP_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY
	if [ $RETVAL -ne 0 ] && [ $FORCE_ACTIVATE -eq 0 ] ;then
		ERROR_EXIT "[FATAL]:-Failed to shutdown gpsyncmaster process" 2;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

ADMIN_SHUTDOWN () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Stopping standby master instance"
	$MPPSTOP -m -d $MASTER_STANDBY_DATA_DIRECTORY
	ERROR_CHK $? "shutdown standby master instance" 1
	if [ x"" != x"$1" ];then
		LOG_MSG "[INFO]:-Re-starting sync process"
		$MPPSYNCMASTER -D $MASTER_STANDBY_DATA_DIRECTORY -p $MASTER_STANDBY_PORT -S
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_CATALOG_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Starting standby master instance in admin mode"
	$MPPSTART -bm -d $MASTER_STANDBY_DATA_DIRECTORY
	ERROR_CHK $? "start standby master in admin mode" 2
	MASTER_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select min(dbid) from $CONFIG_TABLE where content=-1;"`
	MASTER_HOSTNAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where dbid in ($MASTER_DBID)"`
	ERROR_CHK $? "obtain master hostname" 1
	if [ $RETVAL -ne 0 ];then 
		ADMIN_SHUTDOWN 1
		ERROR_EXIT "[FATAL]:-Unable to obtain master hostname" 2;fi
	MASTER_PORT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select port from $CONFIG_TABLE where dbid=$MASTER_DBID;"`
	ERROR_CHK $? "obtain master port number" 1
	if [ $RETVAL -ne 0 ];then 
		ADMIN_SHUTDOWN 1
		ERROR_EXIT "[FATAL]:-Unable to obtain master port" 2;fi
	PING_HOST $MASTER_HOSTNAME
	if [ $RETVAL -eq 0 ];then
		LOG_MSG "[INFO]:-Master host $MASTER_HOSTNAME can be contacted, making further checks"
		GET_PG_PID_ACTIVE $MASTER_PORT $MASTER_HOSTNAME
		if [ $PID -ne 0 ];then
			LOG_MSG "[FATAL]:-Appears that there is an active postmaster on $MASTER_HOST port=$MASTER_PORT"
			ADMIN_SHUTDOWN 1
			ERROR_EXIT "[FATAL]:-Unable to continue, master instance still appears to be active" 2
		fi
	fi	
	MASTER_STANDBY_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select dbid from $CONFIG_TABLE where content=-1 and definedprimary='f' and isprimary='f';"`
	ERROR_CHK $? "obtain standby dbid" 2
	LOG_MSG "[INFO]:-Obtained standby master dbid as $MASTER_STANDBY_DBID"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Master standby: $MASTER_STANDBY_HOSTNAME"
	MASTER_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"select dbid from $CONFIG_TABLE where content=-1 and definedprimary='t' and isprimary='t';"`
	LOG_MSG "[INFO}:-Obtained MASTER_DBID as $MASTER_DBID"
	env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"update $CONFIG_TABLE set hostname='$MASTER_STANDBY_HOSTNAME' where dbid=$MASTER_DBID ;"
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
	    ADMIN_SHUTDOWN
	    ERROR_EXIT "[FATAL]:-Unable to contine, unable to update $CONFIG_TABLE catalog" 2
	fi
	LOG_MSG "[INFO]:-Deleting original master host details"
	env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_STANDBY_PORT $DEFAULTDB -A -t -c"delete from $CONFIG_TABLE where dbid=$MASTER_STANDBY_DBID;"
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		ADMIN_SHUTDOWN
		ERROR_EXIT "[FATAL]:-Unable to contine, unable to update $CONFIG_TABLE catalog" 2
	fi
	LOG_MSG "[INFO]:-Updating dbid value of standby master instance catalog line"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_STANDBY_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears standby instance shutdown during this process, check standby instance log file" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of standby instance, including a call to shutdown segment instances"
		LOG_MSG "[INFO]:-Ignore any warning messages if mppstop.sh reports problems with shutdown of segment instances"
		$MPPSTOP -qfbya -d $MASTER_STANDBY_DATA_DIRECTORY
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP standby master activation parameters"
	LOG_MSG "[INFO]:-----------------------------------------"
	LOG_MSG "[INFO]:-MPP Master standby hostname       = $MASTER_STANDBY_HOSTNAME"
	LOG_MSG "[INFO]:-MPP Master standby port           = $MASTER_STANDBY_PORT"
	LOG_MSG "[INFO]:-MPP Master standby data directory = $MASTER_STANDBY_DATA_DIRECTORY"
	if [ x"" == x"$NEW_STANDBY_MASTER" ];then
		LOG_MSG "[WARN]:-MPP Build new Master standby      = Off"
	else
		LOG_MSG "[INFO]:-MPP Build new Master standby      = On"
		LOG_MSG "[INFO]:-MPP new Master standby hostname   = $NEW_STANDBY_MASTER"
	fi
	if [ $FORCE_ACTIVATE -eq 1 ];then
		LOG_MSG "[WARN]:-Force Master standby activation   = On"
	else
		LOG_MSG "[INFO]:-Force Master standby activation   = Off"
	fi
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
	LOG_MSG "[INFO]:-Starting the MPP instance"
	$MPPSTART -d $MASTER_STANDBY_DATA_DIRECTORY -ay
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_INFO () {
	LOG_MSG "[INFO]:-The activation of the standby master has completed successfully."
	LOG_MSG "[INFO]:-$MASTER_STANDBY_HOSTNAME is now the new master master."
	LOG_MSG "[INFO]:-You will need to update your user access mechanism to reflect"
	LOG_MSG "[INFO]:-the change of master hostname."
	LOG_MSG "[INFO]:-Do not re-start the failed master whilst the fail-over master is"
	LOG_MSG "[INFO]:-operational, this could result in database corruption!"
	LOG_MSG "[INFO]:-MASTER_DATA_DIRECTORY is now $MASTER_STANDBY_DATA_DIRECTORY if"
	LOG_MSG "[INFO]:-this has changed as a result of the standby master activation, rememeber"
	LOG_MSG "[INFO]:-to change this in any startup scripts etc, that may be configured"
	LOG_MSG "[INFO]:-to set this value." 
	if [ x"" == x"$NEW_STANDBY_MASTER" ];then
	LOG_MSG "[WARN]:-New standby master not built"
	else
	LOG_MSG "[INFO]:-New standby Master Instance $NEW_STANDBY_MASTER"
	fi
	LOG_MSG "[INFO]:-MASTER_PORT is now $MASTER_STANDBY_PORT, if this has changed, you"
	LOG_MSG "[INFO]:-may need to make additional configuration changes to allow access"
	LOG_MSG "[INFO]:-to the MPP instance."
	LOG_MSG "[INFO]:-When the failed master becomes available, run mppactivatemaster.sh"
	LOG_MSG "[INFO]:-to re-activate the master and return the standby master to its previous"
	LOG_MSG "[INFO]:-state."
	LOG_MSG "[INFO]:-Statistics will need to be run against the database"
	LOG_MSG "[INFO]:-Following standby master activation"
	LOG_MSG "[INFO]:-When convenient, run analyze against the user databases"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":v'?'ac:ql:d:f!" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			! ) SCRIPT_USAGE=1
			    USAGE ;;
			d ) MASTER_STANDBY_DATA_DIRECTORY=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			f ) FORCE_ACTIVATE=1 ;;
			c ) NEW_STANDBY_MASTER=$OPTARG ;;
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
if [ x"" == x"$NEW_STANDBY_MASTER" ];then
	LOG_MSG "[WARN]:-New master standby host build request not provided via -c option"
	LOG_MSG "[WARN]:-MPP Master Instance does not have a fail-over"
	LOG_MSG "[INFO]:-Run mppinitstandby.sh to create a standby master instance"
	EXIT_STATUS=1
else
	LOG_MSG "[INFO]:-Processing request to create a new standby master instance"
	$MPPINITSTANDBY -s $NEW_STANDBY_MASTER -a
fi
DISPLAY_INFO
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
