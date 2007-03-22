#!/bin/bash
#	Filename:-		gpactivatemaster.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released.
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Re-activates the Master segment instance.
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
CWORKDIR=`pwd`
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
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
FORCE_ACTIVATE=0
unset START_gpsync
REMOTE_GPSTART=$GPSTART
REMOTE_gpstop=$gpstop
REMOTE_INIT_STANDBY=$GPINITSTANDBY
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Re-activates a failed master segment instance."
		$ECHO "      Used when a standby master instance has been activated as a result of a"
		$ECHO "      failure of the master segment instance host."
		$ECHO "      Script will take the following actions:"
		$ECHO "      1. Stop the MPP instance running on the active standby segment instance"
		$ECHO "      2. Copy the standby segment instance data directory to the master segment instance"
		$ECHO "      3. Update the master catalog table"
		$ECHO "      4. Initialize the standby master segment instance"
		$ECHO "      5. Re-start the master instance on the master segment instance"
		$ECHO "      Script should be run on the active standby segment instance host, and expects"
		$ECHO "      the MPP instance to be active, and the master data directory to be"
		$ECHO "      the same as that of the active standby segment instance"
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
		$ECHO "      -a, don't ask to confirm master segment instance re-activation [default:- ask]"
		$ECHO
		$ECHO "      Master re-activation options:"
		$ECHO "      -m, <master hostname> [mandatory]" 
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but master QD master database operational"
		$ECHO "      2 Fatal error, master master QD database not operational, or in an inconsistent state"
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
	MASTER_HOSTNAME=`$HOSTNAME`
	#Check that we don't have an gpsyncmaster process running on this host
	if [ `$GREP -c gpsyncmaster $MASTER_DATA_DIRECTORY/$PG_OPT` -eq 1 ] && [ -f $MASTER_DATA_DIRECTORY/$PG_PID ]
	then
		ERROR_EXIT "[FATAL]:-Sync master process still active on this host" 2
	fi
	#Check that the cluster is active
	CHK_DB_RUNNING
	#Get the standby master port number
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	#Check that we are in a active standby situation
	ACTIVE_COUNT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from $CONFIG_TABLE where definedprimary='f' and content=-1 and hostname='${MASTER_HOSTNAME}';"`
	ERROR_CHK $? "obtain active standby count" 2
	if [ $ACTIVE_COUNT -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Not in master fail-back situation" 2
	fi
	#Obtain the fail-back master port number
	FAILBACK_MASTER_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select min(dbid) from $CONFIG_TABLE where content=-1;"`
	FAILBACK_MASTER_PORT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select port from $CONFIG_TABLE where dbid in ($FAILBACK_MASTER_DBID);"`
	ERROR_CHK $? "obtain fail-back master port number" 2
	#Obtain the fail-back master data directory
	FAILBACK_MASTER_DATA_DIRECTORY=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select datadir from $CONFIG_TABLE where dbid in ($FAILBACK_MASTER_DBID);"`
	ERROR_CHK $? "obtain fail-back master data directory" 2
	#Check to ensure that we have an entry for FAILBACK_MASTER_HOSTNAME in $CONFIG_TABLE table
##	FAILBACK_MASTER_HOSTID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select 1 from $CONFIG_TABLE where hostname='${FAILBACK_MASTER_HOSTNAME}';"`
##	ERROR_CHK $? "obtain fail-back master check" 2
	#Check that we are not trying to re-activate on the standby host
	if [ "$FAILBACK_MASTER_HOSTNAME" == "`$HOSTNAME`" ];then
		ERROR_EXIT "[FATAL]:-Must run this script on the standby master host" 2
	fi
	#Check to see if we can establish contact with the master host
	PING_HOST $FAILBACK_MASTER_HOSTNAME
	if [ $RETVAL -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Unable to establish contact with $FAILBACK_MASTER_HOSTNAME" 2
	fi
	#Check to see if the master host data directory already exists 
	CHK_DIR $FAILBACK_MASTER_DATA_DIRECTORY $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 1 ];then
		LOG_MSG "[INFO]:-Master data directory $FAILBACK_MASTER_DATA_DIRECTORY on $FAILBACK_MASTER_HOSTNAME does not exist"
	else
		LOG_MSG "[WARN]:-Master data directory $FAILBACK_MASTER_DATA_DIRECTORY on $FAILBACK_MASTER_HOSTNAME exists, contents will be over written!"	
		EXIT_STATUS=1
	fi
	#Check to ensure that we do not have a postmaster process running on the primary QD host
	CHK_FILE $FAILBACK_MASTER_DATA_DIRECTORY/$PG_PID $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 1 ];then
		ERROR_EXIT "[FATAL]:-File $FAILBACK_MASTER_DATA_DIRECTORY/$PG_PID exists on $FAILBACK_MASTER_HOSTNAME" 2
	fi	
	#Check that we have the required additional scripts
	if [ ! -f $GPSTART ];then ERROR_EXIT "[FATAL $GPSTART does not exist" 2;fi
	if [ ! -f $gpstop ];then ERROR_EXIT "[FATAL $gpstop does not exist" 2;fi
	#Also check that we have these on the master host to be re-activated
	CHK_FILE $REMOTE_GPSTART $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 1 ];then
		ERROR_EXIT "[FATAL]:-File $REMOTE_GPSTART does not exist on $FAILBACK_MASTER_HOSTNAME" 2
	fi
	CHK_FILE $REMOTE_gpstop $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 1 ];then
		ERROR_EXIT "[FATAL]:-File $REMOTE_gpstop does not exist on $FAILBACK_MASTER_HOSTNAME" 2
	fi
	CHK_FILE $REMOTE_INIT_STANDBY $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 1 ];then
		ERROR_EXIT "[FATAL]:-File $REMOTE_INIT_STANDBY does not exist on $FAILBACK_MASTER_HOSTNAME" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP Master Segment re-activation parameters"
	LOG_MSG "[INFO]:--------------------------------------------"
	LOG_MSG "[INFO]:-MPP Master standby hostname       = $MASTER_HOSTNAME"
	LOG_MSG "[INFO]:-MPP Master standby port           = $MASTER_PORT"
	LOG_MSG "[INFO]:-MPP Master standby data directory = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-MPP Master re-activate hostname   = $FAILBACK_MASTER_HOSTNAME"
	LOG_MSG "[INFO]:-MPP Master re-activate port       = $FAILBACK_MASTER_PORT"
	LOG_MSG "[INFO]:-MPP Master re-activate directory  = $FAILBACK_MASTER_DATA_DIRECTORY"
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP Master re-activation Yy/Nn>"
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

PROCESS_REACTIVATION () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing shutdown of instance"
	$gpstop -qafy
	ERROR_CHK $? "[FATAL]:-Issues with instance shutdown, please correct and re-run this script" 2
	LOG_MSG "[INFO]:-Starting master instance in admin mode"
	$GPSTART -m
	ERROR_CHK $? "starting master instance in admin mode" 2
	LOG_MSG "[INFO]:-Updating system catalog tables"
	UPDATE_CATALOG
	ADMIN_SHUTDOWN	
	CHK_DIR $FAILBACK_MASTER_DATA_DIRECTORY $FAILBACK_MASTER_HOSTNAME
	if [ $EXISTS -eq 0 ];then
		LOG_MSG "[INFO]:-Removing $FAILBACK_MASTER_DATA_DIRECTORY on $FAILBACK_MASTER_HOSTNAME"
		RUN_COMMAND_REMOTE $FAILBACK_MASTER_HOSTNAME "$RM -Rf $FAILBACK_MASTER_DATA_DIRECTORY"
	fi
	LOG_MSG "[INFO]:-Commencing remote copy of $MASTER_DATA_DIRECTORY to $FAILBACK_MASTER_HOSTNAME"
	$TRUSTED_COPY -pr $MASTER_DATA_DIRECTORY ${FAILBACK_MASTER_HOSTNAME}:${FAILBACK_MASTER_DATA_DIRECTORY} 2>>$LOG_FILE
	ERROR_CHK $? "remote copy $MASTER_DATA_DIRECTORY to $FAILBACK_MASTER_HOSTNAME" 2
##	LOG_MSG "[INFO]:-Commencing re-sync of standby"
##	$TRUSTED_SHELL $FAILBACK_MASTER_HOSTNAME "export MASTER_DATA_DIRECTORY=$FAILBACK_MASTER_DATA_DIRECTORY;$REMOTE_INIT_STANDBY -s $MASTER_HOSTNAME -i -na"
##	ERROR_CHK $? "re-sync of standby QD" 1
	LOG_MSG "[INFO]:-Removing standby postmaster.opts file"
	$RM -f $MASTER_DATA_DIRECTORY/$PG_OPT
##	LOG_MSG "[INFO]:-Restarting sync process on $MASTER_HOSTNAME"
##	RUN_COMMAND_REMOTE $FAILBACK_MASTER_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;. $SCRIPTDIR/gp_bash_functions.sh; START_SYNC_PROCESS $MASTER_HOSTNAME $MASTER_DATA_DIRECTORY $MASTER_PORT"
	LOG_MSG "[INFO]:-Starting MPP instance on $FAILBACK_MASTER_HOSTNAME"
	RUN_COMMAND_REMOTE $FAILBACK_MASTER_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$REMOTE_GPSTART -d $FAILBACK_MASTER_DATA_DIRECTORY -a"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	FAILBACK_MASTER_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select min(dbid) from $CONFIG_TABLE where content=-1;"`
###	env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT -A -t -c"update $CONFIG_TABLE set hostname='$FAILBACK_MASTER_HOSTNAME' where dbid in ($FAILBACK_MASTER_DBID);"
	env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"update $CONFIG_TABLE set hostname='$FAILBACK_MASTER_HOSTNAME' where dbid in ($FAILBACK_MASTER_DBID);"
	if [ $RETVAL -ne 0 ];then
	    ADMIN_SHUTDOWN
	    ERROR_EXIT "[FATAL]:-Unable to contine, unable to update $CONFIG_TABLE catalog" 2
	fi
	NEW_FAIL_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(dbid)+1 from $CONFIG_TABLE ;"`
	LOG_MSG "[INFO]:-Obtained new master standby dbid $NEW_FAIL_DBID"
	LOG_MSG "Creating new Master standby catalog record"
	env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"insert into $CONFIG_TABLE values (-1,'f',$NEW_FAIL_DBID,'f','t',$MASTER_HOSTNAME,$MASTER_PORT,'$MASTER_DATA_DIRECTORY');"
	ERROR_CHECK $? "insert new standby catalog record" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
ADMIN_SHUTDOWN () {
        LOG_MSG "[INFO]:-Start Function $FUNCNAME"
        LOG_MSG "[INFO]:-Stopping master segment instance"
        $gpstop -m
        ERROR_CHK $? "shutdown master segment instance" 1
        LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
LOG_MSG "[INFO]:-This utility has been deprecated"
LOG_MSG "[INFO]:-Use gpactivatestandby.sh and gpinitstandby.sh"
LOG_MSG "[INFO]:-Review usage via -? for both utilities"
exit 0
if [ $# -ne 0 ]; then
	while getopts ":v'?'aqm:l:" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			m ) FAILBACK_MASTER_HOSTNAME=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
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
CHK_DIR $MASTER_STANDBY_DATA_DIRECTORY
if [ $EXISTS -eq 1 ];then
        ERROR_EXIT "[FATAL]:-Standby data directory $MASTER_STANDBY_DATA_DIRECTORY does not exist on this host" 2
fi
CHK_ON_PASSIVE_STANDBY
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" == x"$FAILBACK_MASTER_HOSTNAME" ];then
	ERROR_EXIT "[FATAL]:-Must supply -m <fail-back master hostname>" 2
fi
LOG_MSG "[INFO]:-Start Main"
CHECK_PARAM
if [ x"" != x"$INTERACTIVE" ];then 
	DISPLAY_PARAM
fi
PROCESS_REACTIVATION
LOG_MSG "[INFO]:-Successfully completed master instance re-activation"
LOG_MSG "[INFO]:-Statistics will need to be run against the database"
LOG_MSG "[INFO]:-Following master instance re-activation"
LOG_MSG "[INFO]:-When convenient, run analyze against the user databases"
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
