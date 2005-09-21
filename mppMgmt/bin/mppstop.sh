#!/bin/bash
#	Filename:-		mppstop.sh
#	Version:-		1.0
#	Updated:-		22/07/2005
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
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$TABECHO	
		$TABECHO "$0 "
		$TABECHO "Stops the MPP database, or stops the Master database from maintenance mode"
		$TABECHO
		$TABECHO $0 "[OPTIONS]"
		$TABECHO
		$TABECHO "General options:"
		$TABECHO "-?, display this help message & exit"
		$TABECHO "-v, display version information & exit"
		$TABECHO "-t, display default parameter settings & exit"
		$TABECHO
		$TABECHO "Logging options:"
		$TABECHO "-q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$TABECHO "-l, <logfile directory> alternative logfile directory [optional]"
		$TABECHO "-a, don't ask to confirm shutdown [default:- ask]"
		$TABECHO
		$TABECHO "Connection options:"
		$TABECHO "-d, <Master database directory> name of alternative Master database to shutdown [optional]"
		$TABECHO
		$TABECHO "Database shutdown options:"
		$TABECHO "-f, Fast shutdown, active transactions interrupted and rolled back"
		$TABECHO "-i, Immediate shutdown, active transaction aborted [Use this option with care]"
		$TABECHO "-s, Smart shutdown, wait for active transaction to complete [default]"
		$TABECHO "-n, Do not wait for shutdown return code of each node [default:- wait]"
		##MPP	$TABECHO "-k, Attempt to kill any orphaned postmaster processes on nodes" 
		##MPP	$TABECHO "-m, Stop QD started in maintenance mode"
		$TABECHO
		$TABECHO "Return codes:"
		$TABECHO "0 No problems encountered with requested operation"
		$TABECHO "1 Warning generated, but database shutdown"
		$TABECHO "2 Fatal error, database not shutdown, or in an inconsistent state"
		$TABECHO
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
		$ECHO "Master database port            = $MASTER_PORT      [Override with -p <port number>]"
		$ECHO "Master database directory       = $MASTER_DATA_DIRECTORY [Override with -d <Master directory>]"
		$ECHO "Default shutdown mode       = $MODE           [Override with -f or -i]"
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
				$ECHO "Wait for shutdown           = True"
		else
				$ECHO "Wait for shutdown           = False"
		fi 
		$ECHO
		exit 0
}

STOP_ADMIN () {
		##MPP Disabled call to this function 22/07/2005
		LOG_MSG "[INFO]:-Stopping the head in admin mode"
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:- No $PG_PID file, is database already stopped?" 1
		fi
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m smart      >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Could not shutdown Master database in admin mode" 2
		fi
		LOG_MSG "[INFO]:-Successfully shutdown the Master in admin mode"
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
		$TABECHO "Master parameters" |$TEE -a $LOG_FILE
		$TABECHO "-----------------" |$TEE -a $LOG_FILE
		$TABECHO "Master MPP database process active PID = $QD_PID" |$TEE -a $LOG_FILE
		$TABECHO "Database                               = $QD_DBNAME" |$TEE -a $LOG_FILE
		$TABECHO "Master port                            = $MASTER_PORT" |$TEE -a $LOG_FILE
		$TABECHO "Master directory                       = $MASTER_DATA_DIRECTORY" |$TEE -a $LOG_FILE
		$TABECHO "Shutdown mode                          = $MODE" |$TEE -a $LOG_FILE
		$TABECHO |$TEE -a $LOG_FILE
		$TABECHO "QE instances that will be shutdown:" |$TEE -a $LOG_FILE
		$TABECHO "-----------------------------------" |$TEE -a $LOG_FILE
		if [ ${#QE_ARRAY[@]} -eq 0 ]; then
				LOG_MSG "[WARN]:-Failed to obtain any QE node details, will only be able to shutdown QE host"
				LOG_MSG "[WARN]:-This may indicate an inconsistent MPP database state"
				EXIT_STATUS=1
		else
				COUNT=1
				for QNODE in "${QE_ARRAY[@]}"
						do
						QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
						QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
						QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
						##MPP		TAIL_VALID=`$ECHO $TNODE|$CUT -d"|" -f3`
						##MPP		$TABECHO "Tail .. $TAIL_NAME directory $TAIL_DIR status $TAIL_VALID"
						$TABECHO "QE [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT" |$TEE -a $LOG_FILE
						((COUNT=$COUNT+1))
				done
		fi
		$ECHO
		$ECHO -n "         Continue with MPP database shutdown yY|nN >"
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
		#Shutdown QE hosts first
		QD_STOP_COUNT=0
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				##MPP		TAIL_VALID=`$ECHO $TNODE|$CUT -d"|" -f3`
				LOG_MSG "[INFO]:-Stopping QE $QE_NAME directory $QE_DIR"
				##MPP		if [ $TAIL_VALID != 't' ] && [ $KILL_INVALID != 'true' ]; then
				##MPP		  LOG_MSG "[WARN]:-Host $TAIL_NAME segment $TAIL_DIR marked as invalid, will not try to stop instance"
				##MPP		else
				$TRUSTED_SHELL $QE_NAME "$PG_CTL $WAIT stop -D $QE_DIR -m $MODE -l ${QE_DIR}.log " >/dev/null 2>&1
				RETVAL=$?
				##MPP		  if [ $RETVAL -ne 0 ] && [ $KILL_INVALID != 'true' ]; then
				if [ $RETVAL -ne 0 ]; then
						LOG_MSG "[WARN]:-Issue with shutdown of $QE_NAME QE database"
						EXIT_STATUS=1
				else
						LOG_MSG "[INFO]:-Successfully shutdown QE Host $QE_NAME database"
				fi
				((QD_STOP_COUNT=$QD_STOP_COUNT+1))
				##MPP		fi
		done
		LOG_MSG "[INFO]:-Stopping Master $QD_NAME directory $MASTER_DATA_DIRECTORY"
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE -l ${MASTER_DATA_DIRECTORY}.log >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Issue with shutdown of Master database" 2
		else
				LOG_MSG "[INFO]:-------------------------------------------------------"
				LOG_MSG "[INFO]:-Successfully shutdown $QD_STOP_COUNT QE instances and Master instance"
		fi
}
#******************************************************************************
# Main Section
#******************************************************************************
#Check to see if we have a parameter supplied to script
if [ $# -ne 0 ]; then
		##MPP	while getopts ":viskft'?'aqnml:d:" opt;do
		while getopts ":viskft'?'aqnl:d:" opt;do
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
						##MPP		m ) ADMIN=1 ;;
						n ) WAIT="-W" ;; 
						l ) LOG_DIR=$OPTARG ;;
						* ) UNKNOWN_OPTION ;;
				esac
		done	
fi

LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
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
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
