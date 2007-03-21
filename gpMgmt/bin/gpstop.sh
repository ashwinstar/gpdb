#!/bin/bash
#	Filename:-		gpstop.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Stops the MPP instance.
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
FUNCTIONS=$WORKDIR/gp_bash_fuctions.sh
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
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`$HOSTNAME`
EXIT_STATUS=0
KILL_INVALID=false
ADMIN=0
STOP_INVALID=0
STANDBY_COUNT=0
STOP_SYNC=1
gpsegSTOP=gpsegstop.sh
PARALLEL_STATUS_FILE=/tmp/gpstop_parallel_status_file.$$
WAIT_LIMIT=600
BATCH_DEFAULT=60
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
		$ECHO "      `basename $0` "
		$ECHO
		$ECHO "      Stops the MPP instance, or stops the Master instance from maintenance mode"
		$ECHO
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
		$ECHO "      -a, don't ask to confirm shutdown [default:- ask]"
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master instance directory> name of alternative Master instance to shutdown [optional]"
		$ECHO
		$ECHO "      Instance shutdown options:"
		$ECHO "      -f, Fast shutdown, active transactions interrupted and rolled back"
		$ECHO "      -i, Immediate shutdown, active transaction aborted [Use this option with care]"
		$ECHO "      -s, Smart shutdown, wait for active transaction to complete [default]"
		$ECHO "      -z, Force shutdown of segment instances marked as invalid. Kill postmaster PID,"
		$ECHO "          delete /tmp lock files and remove segment instance postmaster.pid file."
		$ECHO "      -r, Restart MPP instance after successful gpstop.sh."
		$ECHO "      -m, Stop master instance started in maintenance mode"
		$ECHO "      -y, Do not stop master standby process [Default stop standby process]"
		$ECHO "      -u, upload new master postgresql.conf settings, does not stop MPP array,"
		$ECHO "          issues a signal to the master segment postmaster process to reload"
		$ECHO "      -B, <number> run this batch of stop segment processes in parallel [default $BATCH_DEFAULT]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but instance shutdown"
		$ECHO "      2 Fatal error, instance not shutdown, or in an inconsistent state"
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
		#Check to see if postgres env variables set within calling users environment
		if [ -z $MASTER_PORT ]; then
				MASTER_PORT="Not set in ENV"
		fi
		if [ -z $MASTER_DATA_DIRECTORY ]; then
				MASTER_DATA_DIRECTORY="Not set in ENV"
		fi
		$ECHO
		$ECHO "Current Default Shutdown Parameters"
		$ECHO "-----------------------------------"
		$ECHO "Master instance port           = $MASTER_PORT  [Override with -p <port number>]"
		$ECHO "Master instance directory      = $MASTER_DATA_DIRECTORY  [Override with -d <Master directory>]"
		$ECHO "Default shutdown mode          = $MODE           [Override with -f or -i]"
		if [ $INTERACTIVE ]; then
				$ECHO "Interactive                    = True            [Override with -a]"
		else
				$ECHO "Interactive                    = False"
		fi
		if [ $VERBOSE ]; then
				$ECHO "Verbose                        = True            [Override with -q]"
		else
				$ECHO "Verbose                        = False"
		fi 
		$ECHO
		exit 0
}

STOP_ADMIN () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		LOG_MSG "[INFO]:-Stopping the Master instance in admin mode"
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:- No $PG_PID file, is instance already stopped?" 1
		fi
		env PGOPTIONS="-c gp_session_role=utility" $PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE      >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master instance from admin mode" 2
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_QD_DB_RUNNING () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:- No $PG_PID file, is MPP instance already stopped?" 2
		else
			QD_PID=`$HEAD -1 $MASTER_DATA_DIRECTORY/$PG_PID`
			QD_PID_DIR=`$HEAD -2 $MASTER_DATA_DIRECTORY/$PG_PID|$TAIL -1`
			#Now check to see if the QD_PID is active
			GET_PG_PID_ACTIVE $MASTER_PORT
			if [ $PID -eq 0 ];then
				LOG_MSG "[WARN]:-Have a $PG_PID file but no Master segment process running" 1
				EXIT_STATUS=1
				LOG_MSG "[INFO]:-Clearing $PG_PID and /tmp lock files" 	 1
				$RM -f $MASTER_DATA_DIRECTORY/$PG_PID
				ERROR_CHK $? "delete Master $PG_PID" 2
				$RM -f /tmp/.s.PGSQL.${MASTER_PORT}*
				ERROR_CHK $? "delete Master /tmp lock files" 2
				LOG_MSG "[INFO]:-Starting recovery procedure" 1
				LOG_MSG "[INFO]:-Starting Master instance in utility mode" 1
				$MPPSTART -mq -d $MASTER_DATA_DIRECTORY
				LOG_MSG "[INFO]:-Setting recovery parameters" 1
				MODE=fast
				unset INTERACTIVE
				STOP_INVALID=1
				LOG_MSG "[INFO]:-Commencing forced shutdown" 1
			fi
		fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROMPT_CONTINUE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		$ECHO
		LOG_MSG "[INFO]:-Master instance parameters:" 1
		LOG_MSG "[INFO]:----------------------------" 1
		LOG_MSG "[INFO]:-Master MPP instance process active PID = $QD_PID" 1
		LOG_MSG "[INFO]:-Database                               = $QD_DBNAME" 1
		LOG_MSG "[INFO]:-Master port                            = $MASTER_PORT" 1
		LOG_MSG "[INFO]:-Master directory                       = $MASTER_DATA_DIRECTORY" 1
		LOG_MSG "[INFO]:-Shutdown mode                          = $MODE" 1
		if [ $STOP_INVALID -eq 1 ];then
		LOG_MSG "[INFO]:-Invalid segment shutdown               = On <<<<<" 1
		else
		LOG_MSG "[INFO]:-Invalid segment shutdown               = Off" 1
		fi
		if [ x"" == x"$STOP_SYNC" ];then
		LOG_MSG "[INFO]:-Shutdown Master standby host           = Off" 1
		else
		LOG_MSG "[INFO]:-Shutdown Master standby host           = On" 1
		fi
		LOG_MSG "[INFO]:-Segment instances that will be shutdown:" 1
		LOG_MSG "[INFO]:-----------------------------------------" 1
		if [ ${#QE_ARRAY[@]} -eq 0 ]; then
				LOG_MSG "[WARN]:-Failed to obtain any Segment instance details, will only be able to shutdown Master host" 1
				LOG_MSG "[WARN]:-This may indicate an inconsistent MPP database state" 1
				EXIT_STATUS=1
		else
				COUNT=1
				for QNODE in "${QE_ARRAY[@]}"
						do
						QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
						QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
						QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
						QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
						LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT" 1
						((COUNT=$COUNT+1))
				done
		fi
		$ECHO
		$ECHO "Continue with MPP instance shutdown yY|nN >"
		read REPLY
		if [ ! -z $REPLY ]; then
			if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
				exit 1
			fi
		else
			LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
			exit 1
		fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}	

PROCESS_SHUTDOWN () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ $STOP_INVALID -eq 1 ];then
			BATCH_LIMIT=$QE_ARRAY_COUNT
		else
			BATCH_LIMIT=`${EXPORT_LIB_PATH};env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $MPP_PG_VIEW where valid ='t' and content<>-1;"`
		fi
		if [ $MODE == "smart" ]; then
#		count connected processes, but don't count ourself.
		CONNECTED=` env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT template1 -A -t -c "select count(*) from pg_stat_activity where procpid != pg_backend_pid();"`
		if [ $CONNECTED -gt 0 ];then
			LOG_MSG "[WARN]:-There are other connections to this instance, shutdown mode smart aborted" 1
			LOG_MSG "[WARN]:-Either remove connections, or use gpstop.sh -f or -i." 1
			LOG_MSG "[WARN]:-See gpstop.sh -? for all options" 1
			ERROR_EXIT "[FATAL]:-Shutdown options will not close instance database" 2
		else
			LOG_MSG "[INFO]:-No current instance connections identified, will continue with shutdown"
		fi	
		fi
		#Shutdown QD hosts first
		if [ x"" != x"$STOP_SYNC" ];then
			STOP_STANDBY_SERVER
		fi
		LOG_MSG "[INFO]:-Stopping Master $QD_NAME directory $MASTER_DATA_DIRECTORY"
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE -l ${MASTER_DATA_DIRECTORY}.log >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master instance on $QD_NAME" 2
		PARALLEL_SETUP  $PARALLEL_STATUS_FILE
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
				if [ "$QE_VALID" == "t" ] || [ $STOP_INVALID -eq 1 ];then
					if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
					$gpsegSTOP $$ $STOP_INVALID $QE_NAME $QE_PORT $QE_DIR $QE_VALID $MODE $INST_COUNT $LOG_FILE &
					PARALLEL_COUNT $BATCH_LIMIT $BATCH_DEFAULT
				else
					if [ $DEBUG_LEVEL -eq 0 ];then $ECHO;fi
					LOG_MSG "[WARN]:-Not processing $QE_NAME directory $QE_DIR marked invalid" 1
					EXIT_STATUS=1
				fi
		done
	DUMP_STAT=0
	PARALLEL_SUMMARY_STATUS_REPORT $PARALLEL_STATUS_FILE
	if [ $KILLED_COUNT -ne 0 ];then
		EXIT_STATUS=1;DUMP_STAT=1
	fi
	FORCE_CLEAR_WARN=`$GREP -c ":FORCE_CLEAR_WARN" $PARALLEL_STATUS_FILE`
	if [ $FORCE_CLEAR_WARN -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned force clear fail     = $FORCE_CLEAR_WARN $WARN_MARK" 1
		EXIT_STATUS=1;DUMP_STAT=1
	fi
	NO_FORCE_CLEAR=`$GREP -c ":NO_FORCE_CLEAR" $PARALLEL_STATUS_FILE`
	if [ $NO_FORCE_CLEAR -ne 0 ];then
		LOG_MSG "[WARN]:-Total processes returned pg_ctl shutdown fail = $NO_FORCE_CLEAR $WARN_MARK" 1
		EXIT_STATUS=1;DUMP_STAT=1
	fi
	LOG_MSG "[INFO]:-Successfully shutdown $COMPLETED_COUNT of $BATCH_LIMIT segment instances" 1
	if [ $DUMP_STAT -ne 0 ];then
                LOG_MSG "[INFO]:-Securing contents of parallel status file to log file" 1
                $CAT $PARALLEL_STATUS_FILE >> $LOG_FILE
                LOG_MSG "[INFO]:--------------------------------------------------------" 1
                LOG_MSG "[INFO]:-Error messages contained within status file" 1
                $CAT $PARALLEL_STATUS_FILE|$GREP -v "COMPLETED:"
                LOG_MSG "[INFO]:--------------------------------------------------------" 1
        else
                LOG_MSG "[INFO]:-Database successfully shutdown with no errors reported" 1
        fi
        $RM -f $PARALLEL_STATUS_FILE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_STANDBY_SERVER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Processing standby server shutdown" 1
	GET_STANDBY_COUNT
	if [ $STANDBY_COUNT -ne 0 ];then
		GET_STANDBY_DETAILS
		STANDBY_HOSTNAME=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f1`
		STANDBY_DATADIR=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f2`
		STOP_SYNC_PROCESS $STANDBY_HOSTNAME $STANDBY_DATADIR
	else
		LOG_MSG "[INFO]:-No standby master host configured" 1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
RELOAD_PSQL_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Signalling Master postmaster process to reload" 1
	$PG_CTL reload -D $MASTER_DATA_DIRECTORY 
	ERROR_CHK $? "signal master postmaster process" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
#Check to see if we have a parameter supplied to script
if [ $# -ne 0 ]; then
		##MPP	while getopts ":viskft'?'aqnml:d:" opt;do
		while getopts ":vuyrmiskzft'?'aqbl:d:!DB:" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						i ) MODE=immediate ;;
						s ) MODE=smart ;;
						f ) MODE=fast ;;
						u ) RELOAD_PARAM=1 ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						a ) unset INTERACTIVE ;;
						q ) unset VERBOSE ;;
						m ) ADMIN=1 ;;
						z ) STOP_INVALID=1 ;;
						l ) LOG_DIR=$OPTARG ;;
						r ) RESTART=1 ;;
						b ) CHK_STANDBY=1 ;;
						y ) unset STOP_SYNC ;;
						! ) SCRIPT_USAGE=1
      						    USAGE ;;
						D ) DEBUG_LEVEL=1 ;;
						B ) BATCH_DEFAULT=$OPTARG ;;
						* ) UNKNOWN_OPTION ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
		EXIT_STATUS=1
else
	LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" == x"$CHK_STANDBY" ];then
	CHK_ON_PASSIVE_STANDBY
fi
CHK_DB_QD_SET
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
CHK_QD_DB_RUNNING
READ_CHK
if [ $ADMIN -eq 1 ] || [ $READ_COUNT -ne 0 ]; then
		STOP_ADMIN
		exit
fi
if [ x"" != x"$RELOAD_PARAM" ];then
	RELOAD_PSQL_PARAM
	LOG_MSG "[INFO]:-End Main"
	exit
fi
GET_QE_DETAILS
if [ ! -f ${WORKDIR}/$gpsegSTOP ];then
	ERROR_EXIT "[FATAL]:-No $gpsegSTOP program" 2
fi
if [ $INTERACTIVE ]; then
		PROMPT_CONTINUE
fi
PROCESS_SHUTDOWN
if [ $RESTART ]; then
	if [ -f $MPPSTART ]; then
		if [ x"" == x"$VERBOSE" ];then
			if [ x"" == x"$STOP_SYNC" ];then
			$MPPSTART -aqy -B $BATCH_DEFAULT
			else
			$MPPSTART -aq -B $BATCH_DEFAULT
			fi
		else
			if [ x"" == x"$STOP_SYNC" ];then
			$MPPSTART -ay -B $BATCH_DEFAULT
			else
			$MPPSTART -a -B $BATCH_DEFAULT
			fi
		fi
	else
		ERROR_EXIT "[FATAL]:-No $MPPSTART" 2
	fi
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
