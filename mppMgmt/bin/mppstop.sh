#!/bin/bash
#	Filename:-		mppstop.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		MPP Conversion SE Co Internal
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Stops the cluster database.
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
QD_NAME=`$HOSTNAME`
EXIT_STATUS=0
KILL_INVALID=false
ADMIN=0
STOP_INVALID=0
STANDBY_COUNT=0
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0` "
		$ECHO
		$ECHO "      Stops the MPP database, or stops the Master database from maintenance mode"
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
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master database directory> name of alternative Master database to shutdown [optional]"
		$ECHO
		$ECHO "      Database shutdown options:"
		$ECHO "      -f, Fast shutdown, active transactions interrupted and rolled back"
		$ECHO "      -i, Immediate shutdown, active transaction aborted [Use this option with care]"
		$ECHO "      -s, Smart shutdown, wait for active transaction to complete [default]"
		$ECHO "      -n, Do not wait for shutdown return code of each segment instance [default:- wait]"
		$ECHO "      -z, Force shutdown of segment instances marked as invalid. Kill postmaster PID,"
		$ECHO "          delete /tmp lock files and remove segment instance postmaster.pid file."
		$ECHO "      -r, Restart MPP database after successful mppstop.sh."
		$ECHO "      -m, Stop master database started in maintenance mode"
		$ECHO "      -y, Stop master standby process"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but database shutdown"
		$ECHO "      2 Fatal error, database not shutdown, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
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
		$ECHO "Master database port           = $MASTER_PORT  [Override with -p <port number>]"
		$ECHO "Master database directory      = $MASTER_DATA_DIRECTORY  [Override with -d <Master directory>]"
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
		if [ "$WAIT" == "-w" ]; then
				$ECHO "Wait for shutdown              = True            [Override with -n]"
		else
				$ECHO "Wait for shutdown              = False"
		fi 
		$ECHO
		exit 0
}

STOP_ADMIN () {
		##MPP Disabled call to this function 22/07/2005
		LOG_MSG "[INFO]:-Stopping the master QD in admin mode"
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:- No $PG_PID file, is database already stopped?" 1
		fi
		env PGOPTIONS="-c mpp_session_role=utility" $PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m smart      >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master database from admin mode" 2
}

CHK_QD_DB_RUNNING () {
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:- No $PG_PID file, is MPP database already stopped?" 2
		fi
		QD_PID=`$HEAD -1 $MASTER_DATA_DIRECTORY/$PG_PID`
		QD_PID_DIR=`$HEAD -2 $MASTER_DATA_DIRECTORY/$PG_PID|$TAIL -1`
}

PROMPT_CONTINUE () {
		$ECHO
		LOG_MSG "[INFO]:-Master parameters"
		LOG_MSG "-------------------------"
		LOG_MSG "[INFO]:-Master MPP database process active PID = $QD_PID"
		LOG_MSG "[INFO]:-Database                               = $QD_DBNAME"
		LOG_MSG "[INFO]:-Master port                            = $MASTER_PORT"
		LOG_MSG "[INFO]:-Master directory                       = $MASTER_DATA_DIRECTORY"
		LOG_MSG "[INFO]:-Shutdown mode                          = $MODE"
		if [ x"" == x"$STOP_SYNC" ];then
		LOG_MSG "[INFO]:-Shutdown Master standby host           = Off"
		else
		LOG_MSG "[INFO]:-Shutdown Master standby host           = On"
		fi
		LOG_MSG "[INFO]:-Segment instances that will be shutdown:"
		LOG_MSG "----------------------------------------------"
		if [ ${#QE_ARRAY[@]} -eq 0 ]; then
				LOG_MSG "[WARN]:-Failed to obtain any Segment instance details, will only be able to shutdown Master host"
				LOG_MSG "[WARN]:-This may indicate an inconsistent MPP database state"
				EXIT_STATUS=1
		else
				COUNT=1
				for QNODE in "${QE_ARRAY[@]}"
						do
						QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
						QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
						QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
						QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
						LOG_MSG "[INFO]:-Host [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT"
						((COUNT=$COUNT+1))
				done
		fi
		$ECHO
		$ECHO "Continue with MPP database shutdown yY|nN >"
		read REPLY
		if [ ! -z $REPLY ]; then
				if [ $REPLY != Y ] && [ $REPLY != y ]; then
						LOG_MSG "[WARN]:-User abort requested, Script Exits!"
						exit 1
				fi
		else
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
}	

PROCESS_SHUTDOWN () {
		if [ $MODE == "smart" ]; then
		CONNECTED=`$PSQL -p $MASTER_PORT template1 -A -t -c "select count(*) from pg_stat_activity;"`
			if [ $CONNECTED -gt 0 ];then
				LOG_MSG "[WARN]:-There are other connections to this cluster, shutdown mode smart aborted"
				LOG_MSG "[WARN]:-Either remove connections, or use mppstop.sh -f or -i."
				LOG_MSG "[WARN]:-See mppstop.sh -? for all options"
				ERROR_EXIT "[FATAL]:-Shutdown options will not close cluster database" 2
			else
			LOG_MSG "[INFO]:-No current database connections identified, will continue with shutdown"
			fi	
		fi
		#Shutdown QD hosts first
		if [ x"" != x"$STOP_SYNC" ];then
			STOP_STANDBY_SERVER
		fi
		LOG_MSG "[INFO]:-Stopping Master $QD_NAME directory $MASTER_DATA_DIRECTORY"
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE -l ${MASTER_DATA_DIRECTORY}.log >/dev/null 2>&1
		ERROR_CHK $? "shutdown Master instance on $QD_NAME" 2
		QD_STOP_COUNT=0
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				QE_VALID=`$ECHO $QNODE|$CUT -d"|" -f4`
				if [ "$QE_VALID" == "t" ] || [ $STOP_INVALID -eq 1 ];then
					LOG_MSG "[INFO]:-Stopping Segment Instance $QE_NAME directory $QE_DIR"
					$TRUSTED_SHELL $QE_NAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;$PG_CTL $WAIT stop -D $QE_DIR -m $MODE -l ${QE_DIR}.log " 
					ERROR_CHK $? "shutdown of $QE_NAME on port=$QE_PORT" 1
					if [ $STOP_INVALID -eq 1 ]; then       
						FORCE_CLEAR
					fi
					((QD_STOP_COUNT=$QD_STOP_COUNT+1))
				else
					LOG_MSG "[WARN]:-Not processing $QE_NAME directory $QE_DIR marked invalid"
					EXIT_STATUS=1
				fi
		done
}

FORCE_CLEAR () {
	LOG_MSG "[INFO]:-Killing postmaster process on $QE_NAME for port=$QE_PORT"
	GET_PG_PID_ACTIVE $QE_PORT $QE_NAME
	if [ $PID -ne 0 ]; then
		$TRUSTED_SHELL $QE_NAME "$KILL -9 $PID" > $LOG_FILE 2>&1
	else
		LOG_MSG "[INFO]:-No active pid found for $QE_NAME for port=$QE_PORT"
	fi
	LOG_MSG "[INFO]:-Clearing lock files on $QE_NAME for directory $QE_DIR"
	$TRUSTED_SHELL $QE_NAME "$RM -f /tmp/.s.PGSQL.${QE_PORT}.lock /tmp/.s.PGSQL.${QE_PORT}"
	ERROR_CHK $? "removal of lock files on $QE_NAME for directory $QE_DIR" 1
	LOG_MSG "[INFO]:-Clearing postmaster.pid file"
	$TRUSTED_SHELL $QE_NAME "$RM -f ${QE_DIR}/postmaster.pid"
	ERROR_CHK $? "clearing postmaster.pid file on $QE_NAME for directory $QE_DIR" 1
}

STOP_STANDBY_SERVER () {
	LOG_MSG "[INFO]:-Processing standby server shutdown"
	GET_STANDBY_COUNT
	if [ $STANDBY_COUNT -ne 0 ];then
		GET_STANDBY_DETAILS
		STANDBY_HOSTNAME=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f1`
		STANDBY_DATADIR=`$ECHO ${STANDBY_ARRAY[@]}|$CUT -d"|" -f2`
		STOP_SYNC_PROCESS $STANDBY_HOSTNAME $STANDBY_DATADIR
	else
		LOG_MSG "[WARN]:-No standby master host configured"
		EXIT_STATUS=1
	fi
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
#Check to see if we have a parameter supplied to script
if [ $# -ne 0 ]; then
		##MPP	while getopts ":viskft'?'aqnml:d:" opt;do
		while getopts ":vyrmiskzft'?'aqnl:d:" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						i ) MODE=immediate ;;
						s ) MODE=smart ;;
						f ) MODE=fast ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						###GET_HEAD_PORT ;;
						a ) unset INTERACTIVE ;;
						q ) unset VERBOSE ;;
						##MPP		k ) KILL_INVALID=true ;;
						m ) ADMIN=1 ;;
						z ) STOP_INVALID=1 ;;
						n ) WAIT="-W" ;; 
						l ) LOG_DIR=$OPTARG ;;
						r ) RESTART=1 ;;
						y ) STOP_SYNC=1 ;;
						* ) UNKNOWN_OPTION ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
		EXIT_STATUS=1
else
	LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
CHK_ON_PASSIVE_STANDBY
CHK_DB_QD_SET
GET_MASTER_PORT $MASTER_DATA_DIRECTORY

if [ $ADMIN -eq 1 ]; then
		STOP_ADMIN
		exit
fi
CHK_QD_DB_RUNNING
GET_QE_DETAILS
if [ $INTERACTIVE ]; then
		PROMPT_CONTINUE
fi
PROCESS_SHUTDOWN
if [ $RESTART ]; then
	if [ -f $MPPSTART ]; then
		if [ x"" == x"$VERBOSE" ];then
			if [ x"" == x"$STOP_SYNC" ];then
			$MPPSTART -aq
			else
			$MPPSTART -aqy
			fi
		else
			if [ x"" == x"$STOP_SYNC" ];then
			$MPPSTART -a
			else
			$MPPSTART -ay
			fi
		fi
	else
		LOG_MSG "[FATAL]:-No $MPPSTART" 2
	fi
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
