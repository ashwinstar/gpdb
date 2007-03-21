#!/bin/bash
#	Filename:-		mppstart.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Starts the MPP instance.
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`hostname`
EXIT_STATUS=0
declare -a FAILED_LIST_ARRAY
unset FAILED
unset ADMIN
FAILED_COUNT=0
TAIL_COUNT=0
START_INVALID=0
STANDBY_COUNT=0
FORCE=0
START_SYNC=1
STOP_PROG=$MPPSTOP
PARALLEL_STATUS_FILE=/tmp/mppstart_parallel_status_file.$$
MPPSEGSTART=mppsegstart.sh
WAIT_LIMIT=600
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
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Starts the full MPP instance, or Master instance only in utility mode"
		$ECHO
		$ECHO "      Usage:"
		$ECHO "      $0 [OPTIONS]"
		$ECHO
		$ECHO "      General options:"
		$ECHO "      -?, display this help message & exit" 
		$ECHO "      -v, display version information & exit"
		$ECHO "      -t, display default parameter settings & exit"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "      -a, don't ask to confirm startup [default:- ask]"
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master instance directory> name of alternative Master instance to start up [optional]"
		$ECHO
		$ECHO "      Database startup options:"
		$ECHO "      -m, start master instance only in maintenance mode [default:- full MPP startup]"
		$ECHO "      -z, force startup of segment instance marked as invalid"
		$ECHO "      -y, do not start master standby server [default:- start standby server process]"
		$ECHO "      -s, switch master log file, moves current logfile to <log file>.YYYYMMDD"
		$ECHO "      -e, switch segment instance log files, moves current logfile to <log file>.YYYYMMDD"
		$ECHO "          not applicable in master maintenance mode startup"
		$ECHO "      -B, <number> run this batch of start segment processes in parallel [default $BATCH_DEFAULT]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but instance operational"
		$ECHO "      2 Fatal error, instance not started, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

DISPLAY_PARAM () {
		if [ -z $MASTER_PORT ]; then
				MASTER_PORT="Not set in ENV"
		fi
		$ECHO
		$ECHO "Current Default Startup Parameters"
		$ECHO "----------------------------------"
		$ECHO "MPP Master instance port        = $MASTER_PORT      [Override with -p <port number>]"
		$ECHO "MPP Master instance directory   = $MASTER_DATA_DIRECTORY [Override with -d <Master directory>]"
		if [ $INTERACTIVE ]; then
				$ECHO "Interactive                 = True            [Override with -a]"
		else
				$ECHO "Interactive                 = False"
		fi
		if [ $VERBOSE ]; then
				$ECHO "Verbose                     = True            [Override with -q]"
		else
				$ECHO "Verbose                     = False"
		fi 
		if [ "$WAIT" == "-w" ]; then
				$ECHO "Wait for startup            = True            [Override with -n]"
		else
				$ECHO "Wait for startup            = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}


CHK_QD_DB_RUNNING () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [  -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
			LOG_MSG "[WARN]:-$PG_PID file exists on Master, checking if recovery startup required" 1
			RECOVERY_STARTUP
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RECOVERY_STARTUP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing recovery startup checks" 1
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	GET_PG_PID_ACTIVE $MASTER_PORT
	if [ $PID -ne 0 ];then	
		ERROR_EXIT "[FATAL]:-Master instance process running" 2
	fi
	LOG_MSG "[INFO]:-No Master instance process, entering recovery startup mode" 1
	LOG_MSG "[INFO]:-Clearing Master instance lock files" 1
	$RM -f /tmp/.s.PGSQL.${MASTER_PORT}.lock /tmp/.s.PGSQL.${MASTER_PORT}
	LOG_MSG "[INFO]:-Clearing Master instance $PG_PID file" 1
	$RM -f ${MASTER_DATA_DIRECTORY}/$PG_PID
	START_ADMIN
	LOG_MSG "[INFO]:-Commencing forced instance shutdown" 1
	$STOP_PROG -faz
	GET_DETAILS
	PROCESS_STARTUP
	EXIT_STATUS=1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	exit $EXIT_STATUS	
}

SWITCH_MASTER_LOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Moving master segment log file to ${MASTER_DATA_DIRECTORY}.log_$CUR_DATE" 1
	if [ -f ${MASTER_DATA_DIRECTORY}.log ];then
		if [ -f ${MASTER_DATA_DIRECTORY}.log_$CUR_DATE ];then
			LOG_MSG "[INFO]:-Master log file has been previously switched today"
			LOG_MSG "[INFO]:-Will append current master segment the switched log file"
			$CAT ${MASTER_DATA_DIRECTORY}.log >> ${MASTER_DATA_DIRECTORY}.log_$CUR_DATE
			ERROR_CHK $? "complete append to switched log file" 1
			$RM -f ${MASTER_DATA_DIRECTORY}.log
			ERROR_CHK $? "complete delete of current master segment log file" 1
		else
			$MV ${MASTER_DATA_DIRECTORY}.log ${MASTER_DATA_DIRECTORY}.log_$CUR_DATE
			ERROR_CHK $? "complete move of master segment log file" 1
		fi
	else
		LOG_MSG "[WARN]:-No current master segment log file, ignoring log file switch request" 1
		EXIT_STATUS=1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		export PGPORT=$MASTER_PORT
		LOG_MSG "[INFO]:-Starting Master instance in admin mode" 1
		$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT"  start >/dev/null 2>&1
		ERROR_CHK $? "startup Master instance" 2
		READ_CHK
		if [ $READ_COUNT -ne 0 ];then
			unset INTERACTIVE;fi
		LOG_MSG "[INFO]:-Obtaining MPP Master catalog information"
		#Now get tail details
		GET_QE_DETAILS
		QE_COUNT=${#QE_ARRAY[@]}
		if [ x"" != x"$START_SYNC" ];then
			GET_STANDBY_COUNT
			if [ $STANDBY_COUNT -ne 0 ];then
				GET_STANDBY_DETAILS
			fi
		fi
		if [ $START_INVALID -eq 1 ];then
			BATCH_LIMIT=$QE_ARRAY_COUNT
			#Now get value of gp_fault_action
			FAULT_ACTION=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"show gp_fault_action;"`
			if [ $FAULT_ACTION == "continue" ];then
				#Need to update gp_configuration file to set all segments marked as invalid to valid
				LOG_MSG "[INFO]:-Utility requested to attempt startup of segments marked as invalid" 1
				#See if any segments marked as invalid
				INVALID_COUNT=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where valid='f' and content<>-1;"` 
				if [ $INVALID_COUNT -gt 0 ];then
					LOG_MSG "[INFO]:-There are $INVALID_COUNT segments marked as invalid" 1
					#Update these to be valid
					${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"update $CONFIG_TABLE set valid='t' where valid='f' and content<>-1;"
					${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"update $CONFIG_TABLE set isprimary='t' where definedprimary='t' and isprimary='f' and content<>-1;"
					LOG_MSG "[INFO]:-Updated configuration table to mark invalid segments as valid" 1
					#Now reset the associated mirrors
					${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"update $CONFIG_TABLE set isprimary='f' where definedprimary='f' and isprimary='t' and content<>-1;"
					LOG_MSG "[INFO]:-Updated configuration table to reset mirrors to passive" 1
				else
					LOG_MSG "[INFO]:-No segments marked as invalid" 1
				fi
			fi
		else
			BATCH_LIMIT=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $MPP_PG_VIEW where valid ='t' and content<>-1;"`
		fi
		#Gather locale information MPP-972
		LC_COLLATE=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"show lc_collate;"`
		ERROR_CHK $? "obtain lc_collate info" 1
		LOG_MSG "[INFO]:-Master lc_collate=$LC_COLLATE"
		LC_MONETARY=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"show lc_monetary;"`
		ERROR_CHK $? "obtain lc_monetary info" 1
		LOG_MSG "[INFO]:-Master lc_monetary=$LC_MONETARY"
		LC_NUMERIC=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"show lc_numeric;"`	
		ERROR_CHK $? "obtain lc_numeric info" 1
		LOG_MSG "[INFO]:-Master lc_numeric=$LC_NUMERIC"
		#Shutdown the QD database
		LOG_MSG "[INFO]:-Shutting down Master instance" 1
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE	 >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master instance" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_ADMIN () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ x"" != x"$SWITCH_LOG" ];then
			SWITCH_MASTER_LOG
		fi
		LOG_MSG "[INFO]:-Starting master instance in admin mode" 1
		export PGPORT=$MASTER_PORT
		env PGOPTIONS="-c gp_session_role=utility" $PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT " start
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				$TAIL ${MASTER_DATA_DIRECTORY}.log|$TEE -a $LOG_FILE
				ERROR_EXIT "[FATAL]:-Could not start master instance in admin mode" 2
		fi
		LOG_MSG "[INFO]:-Successfully started master instance in admin mode" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


PROMPT_CONTINUE () {
		LOG_MSG "[INFO]:-Master instance parameters:" 1
		LOG_MSG "[INFO]:----------------------------" 1
		LOG_MSG "[INFO]:-Database                   = $QD_DBNAME" 1
		LOG_MSG "[INFO]:-Master Port                = $MASTER_PORT" 1
		LOG_MSG "[INFO]:-Master directory           = $MASTER_DATA_DIRECTORY" 1
		if [ $START_INVALID -eq 1 ];then
		LOG_MSG "[INFO]:-Start invalid segments     = On <<<<<" 1
		else
		LOG_MSG "[INFO]:-Start invalid segments     = Off" 1
		fi
		if [ x"" == x"$SWITCH_LOG" ];then
		LOG_MSG "[INFO]:-Master switch log file     = Off" 1
		else
		LOG_MSG "[INFO]:-Master switch log file     = On" 1
			if [ -f ${MASTER_DATA_DIRECTORY}.log_$CUR_DATE ];then
			LOG_MSG "[INFO]:-Switch type                = Append" 1
			else
			LOG_MSG "[INFO]:-Switch type                = Move" 1
			fi
		fi
		if [ x"" == x"$SWITCH_QE_LOG" ];then
		LOG_MSG "[INFO]:-Segment switch log file    = Off" 1
		else
		LOG_MSG "[INFO]:-Segment switch log file    = On" 1
		fi
		if [ x"" != x"$START_SYNC" ] && [ $STANDBY_COUNT -ne 0 ];then
		LOG_MSG "[INFO]:-Master standby start       = On" 1
		else
		LOG_MSG "[INFO]:-Master standby start       = Off" 1
		fi
		LOG_MSG "[INFO]:-Segment instances that will be started:" 1
		LOG_MSG "[INFO]:----------------------------------------" 1
		COUNT=1
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
				QE_PRIMARY=`$ECHO $QNODE|$CUT -d"|" -f5`
				if [ "$QE_VALID" == "t" ]; then
					LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME segment = $QE_DIR port = $QE_PORT" 1
				else
					if [ $START_INVALID -eq 0 ];then
						LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME segment = $QE_DIR port = $QE_PORT invalid will not start" 1
					else
						LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME segment = $QE_DIR port = $QE_PORT invalid will attempt forced start" 1
					fi
				fi
				((COUNT=$COUNT+1))
		done
		$ECHO
		$ECHO "Continue with MPP instance startup yY|nN >"
		read REPLY
		if [ -z $REPLY ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
				exit 1
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
				exit 1
		fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}	

PROCESS_STARTUP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ x"" != x"$SWITCH_LOG" ];then
		SWITCH_MASTER_LOG
	fi
	if [ $READ_COUNT -eq 0 ];then
		if [ x"" != x"$START_SYNC" ] && [ $STANDBY_COUNT -ne 0 ];then
			STANDBY_HOSTNAME=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f1`
			STANDBY_DATADIR=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f2`
			STANDBY_PORT=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f3`	
			START_SYNC_PROCESS $STANDBY_HOSTNAME $STANDBY_DATADIR $STANDBY_PORT
			if [ $PID -eq 0 ];then
			SYNC_STATUS=1
			else
			SYNC_STATUS=0
			fi
		fi
	fi
	
	LOG_MSG "[INFO]:-Commencing parallel segment instance startup, please wait..." 1
	export LC_COLLATE=$LC_COLLATE
	export LC_MONETARY=$LC_MONETARY
	export LC_NUMERIC=$LC_NUMERIC
	INST_COUNT=0
	if [ x"" == x"$SWITCH_QE_LOG" ];then
		SWITCH_QE_LOG=0
	fi
	PARALLEL_SETUP  $PARALLEL_STATUS_FILE
	for QNODE in "${QE_ARRAY[@]}"
	do
		QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
		QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
		QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
		QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
		QE_PRIMARY=`$ECHO $QNODE|$CUT -d"|" -f5`
		#If valid is false and no request to start invalis segment skip this instance
		if [ $START_INVALID -eq 0 ] && [ $QE_VALID == 'f' ];then
			if [ $DEBUG_LEVEL -eq 0 ];then $ECHO;fi
			LOG_MSG "[INFO]:-Skipping startup of $QE_NAME $QE_DIR as marked as invalid" 1
		else
			if [ $START_INVALID -eq 1 ] && [ $QE_VALID == 'f' ];then
				VALID_SETTING=1
			fi
			if [ $START_INVALID -eq 1 ] && [ $QE_VALID == 't' ];then
				VALID_SETTING=0
			fi
			if [ $START_INVALID -eq 0 ] && [ $QE_VALID == 't' ];then
				VALID_SETTING=0
			fi	
			if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
			$MPPSEGSTART $$ $SWITCH_QE_LOG $QE_NAME $QE_PORT $QE_DIR $QE_VALID $VALID_SETTING $INST_COUNT $LOG_FILE $QD_DBNAME &
			PARALLEL_COUNT $BATCH_LIMIT $BATCH_DEFAULT
		fi
	done
	if [ $DEBUG_LEVEL -eq 0 ];then $NOLINE_ECHO;fi
	DUMP_STAT=0
	#Now check the status of the segment responses in the status file
	PARALLEL_SUMMARY_STATUS_REPORT $PARALLEL_STATUS_FILE
	if [ $KILLED_COUNT -ne 0 ];then
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	INVALID_START_FAIL=`$GREP -c ":INVALID_START_FAIL" $PARALLEL_STATUS_FILE`
	if [ $INVALID_START_FAIL -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned invalid start fail   = $INVALID_START_FAIL $WARN_MARK" 1
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	VALID_START_FAIL=`$GREP -c ":VALID_START_FAIL" $PARALLEL_STATUS_FILE`
	if [ $VALID_START_FAIL -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned valid start fail     = $VALID_START_FAIL $WARN_MARK" 1
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	PG_CTL_NO_RUNNING=`$GREP -c ":PG_CTL_NO_RUNNING" $PARALLEL_STATUS_FILE`
	if [ $PG_CTL_NO_RUNNING -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned pg_ctl not running   = $PG_CTL_NO_RUNNING $WARN_MARK" 1
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	PG_CTL_THERE_START_FAILED=`$GREP -c ":PG_CTL_THERE_START_FAILED" $PARALLEL_STATUS_FILE`
	if [ $PG_CTL_THERE_START_FAILED -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned pg_ctl fail          = $PG_CTL_THERE_START_FAILED $WARN_MARK" 1
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	LC_ERROR=`$GREP -c ":LC_ERROR" $PARALLEL_STATUS_FILE`
	if [ $LC_ERROR -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned lc_error             = $LC_ERROR $WARN_MARK" 1
		EXIT_STATUS=1
		DUMP_STAT=1
	fi
	if [ x"" != x"$START_SYNC" ] && [ $STANDBY_COUNT -ne 0 ];then
		if [ $SYNC_STATUS -ne 0 ];then
			LOG_MSG "[WARN]:-Failed to start Master standby instance" 1
		else
			LOG_MSG "[INFO]:-Successfully started the Master standby instance" 1
		fi
	fi
	if [ $DUMP_STAT -ne 0 ];then
		LOG_MSG "[INFO]:-Securing contents of parallel status file to log file" 1
		$CAT $PARALLEL_STATUS_FILE >> $LOG_FILE
		LOG_MSG "[INFO]:--------------------------------------------------------" 1
		LOG_MSG "[INFO]:-Error messages contained within status file" 1
		$CAT $PARALLEL_STATUS_FILE|$GREP -v "COMPLETED:"
		LOG_MSG "[INFO]:--------------------------------------------------------" 1
	fi
	LOG_MSG "[INFO]:-Successfully started $COMPLETED_COUNT of $BATCH_LIMIT segment instances" 1
	$RM -f $PARALLEL_STATUS_FILE
	LOG_MSG "[INFO]:-Starting Master instance $QD_NAME directory $MASTER_DATA_DIRECTORY" 1
	export PGPORT=${MASTER_PORT}
	export PGPATH=${APP_PATH}
	$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -E -p $MASTER_PORT " start -m $MODE  > /dev/null 2>&1
	ERROR_CHK $? "startup Master instance" 2
	RET_TEXT=`$PG_CTL status -D $MASTER_DATA_DIRECTORY`
	RUNNING=`$ECHO $RET_TEXT|$EGREP -c "not running|neither"`
	if [ $RUNNING -ne 0 ]; then
		LOG_MSG "[WARN]:-Command pg_ctl reports $QD_NAME for directory $MASTER_DATA_DIRECTORY port $MASTER_PORT not running" 1
		EXIT_STATUS=1
	else
		LOG_MSG "[INFO]:-Command pg_ctl reports Master $QD_NAME instance active" 1
	fi
	$PSQL -p $MASTER_PORT $DEFAULTDB -c"\l" >/dev/null 2>&1
	if [ $DUMP_STAT -ne 1 ] && [ $EXIT_STATUS -eq 0 ];then
		LOG_MSG "[INFO]:-Database successfully started with no errors reported" 1
	else
		LOG_MSG "[WARN]:-Database started but warnings, please check log file" 1
	fi 
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$RM -f $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
		while getopts ":vt'?'abqyzsml:d:e!DB:" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						a ) unset INTERACTIVE ;;
						q ) unset VERBOSE ;;
						m ) ADMIN=1 ;;
						s ) SWITCH_LOG=1 ;;
						e ) SWITCH_QE_LOG=1 ;;
						z ) START_INVALID=1 ;;
						l ) LOG_DIR=$OPTARG ;;
						y ) unset START_SYNC ;;
						b ) CHK_STANDBY=1 ;;
						! ) SCRIPT_USAGE=1 
						    USAGE ;;
						D ) DEBUG_LEVEL=1 ;;
						B ) BATCH_DEFAULT=$OPTARG ;;
						* ) USAGE ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" == x"$CHK_STANDBY" ];then
	CHK_ON_PASSIVE_STANDBY
fi
if [ x"" != x"$MPPHOME" ];then
	APP_PATH=${MPPHOME}/bin
else
	APP_PATH=${BIZHOME}/bin
fi
CHK_QD_DB_RUNNING
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
if [ $ADMIN ]; then
	START_ADMIN
	exit $EXIT_STATUS
fi
GET_DETAILS
if [ $INTERACTIVE ]; then
	PROMPT_CONTINUE
fi
if [ ! -f ${WORKDIR}/$MPPSEGSTART ];then
	ERROR_EXIT "[FATAL]:-Parallel mode selected by no ${WORKDIR}/$MPPSEGSTART exists" 2
fi
PROCESS_STARTUP
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
