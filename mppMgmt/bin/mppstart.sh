#!/bin/bash
#	Filename:-		mppstart.sh
#	Version:-		1.0
#	Updated:-		21/07/2005
#	Status:-		MPP Conversion SE Co Internal
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
QD_NAME=`hostname`
EXIT_STATUS=0
declare -a FAILED_LIST_ARRAY
unset FAILED
unset ADMIN
FAILED_COUNT=0
TAIL_COUNT=0
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$TABECHO	
		$TABECHO $0 
		$TABECHO "Starts the full MPP database, or QE only in utility mode"
		$TABECHO "Cannot use -x and -n options together as mutually exclusive"
		$TABECHO
		$TABECHO "Usage:"
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
		$TABECHO "-a, don't ask to confirm startup [default:- ask]"
		$TABECHO
		$TABECHO "Connection options:"
		$TABECHO "-d, <QE database directory> name of alternative QE database to start up [optional]"
		$TABECHO
		$TABECHO "Database startup options:"
		$TABECHO "-n, do not wait for startup return code of each node [default:- wait]"
		##MPP	$TABECHO "-m, start QE only in maintenance mode [default:- full MPP startup]"
		$TABECHO "-x, mark failed segment as down [default:- do not mark segment as down on startup failure]"
		$TABECHO
		$TABECHO "Return codes:"
		$TABECHO "0 No problems encountered with requested operation"
		$TABECHO "1 Warning generated, but database operational"
		$TABECHO "2 Fatal error, database not started, or in an inconsistent state"
		$TABECHO
		exit $EXIT_STATUS
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
		$ECHO "MPP head database port      = $MASTER_PORT      [Override with -p <port number>]"
		$ECHO "MPP head database directory = $MASTER_DATA_DIRECTORY [Override with -d <QE directory>]"
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
		#MPP	if [ $FAILED ]; then
		#MPP	$ECHO "Mark segment as failed      = True"
		#MPP	else
		#MPP	$ECHO "Mark segment as failed      = False           [Override with -x]"
		#MPP	fi
		$ECHO
		exit $EXIT_STATUS
}


CHK_QD_DB_RUNNING () {
		if [  -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:- $PG_PID file exists, is MPP database already running?" 2
		fi
}

GET_DETAILS () {
		#Get the database name
		#Start the head datase in admin mode
		export PGPORT=$MASTER_PORT
		LOG_MSG "[INFO]:-Starting Master database in admin mode"
		##MPP	$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT -c "mpp_mode=off start"" >/dev/null 2>&1
		$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT"  start >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Could not start Master database" 2
		else
				LOG_MSG "[INFO]:-Successfully started Master database"
		fi
		LOG_MSG "[INFO]:-Obtaining MPP QE catalog information"
		#Now get tail details
		##MPP	TAIL_ARRAY=(`$PSQL -p $HEAD_PORT $HEAD_DBNAME -A -t -c"select host,datadir,port,valid from mpp_instance;"`)	
		GET_QE_DETAILS
		QE_COUNT=${#QE_ARRAY[@]}
		#Shutdown the head database
		LOG_MSG "[INFO]:-Shutting down Master database"
		$PG_CTL $WAIT stop -D $MASTER_DATA_DIRECTORY -m $MODE	 >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Could not shutdown Master database" 2
		else
				LOG_MSG "[INFO]:-Successfully shutdown the Master database"
		fi
}

START_ADMIN () {
		LOG_MSG "[INFO]:-Starting head database in admin mode"
		export PGPORT=$HEAD_PORT
		$PG_CTL -w -D $HEAD_DATA_DIRECTORY -l ${HEAD_DATA_DIRECTORY}.log -o "-i -p $HEAD_PORT -c "mpp_mode=off start"" >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				$TAIL ${HEAD_DATA_DIRECTORY}.log|$TEE -a $LOG_FILE
				ERROR_EXIT "[FATAL]:-Could not start head database in admin mode" 2
		fi
		LOG_MSG "[INFO]:-Successfully started head node in admin mode"
}


PROMPT_CONTINUE () {
		$ECHO
		$TABECHO "Master parameters:"
		$TABECHO "------------------"
		$TABECHO "Database           = $QD_DBNAME"
		$TABECHO "Master Port        = $MASTER_PORT"
		$TABECHO "Master directory   = $MASTER_DATA_DIRECTORY"
		$TABECHO "QE instances that will be started:"
		$TABECHO "----------------------------------"
		COUNT=1
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				$TABECHO "QE [$COUNT] $QE_NAME data directory = $QE_DIR port = $QE_PORT"
				((COUNT=$COUNT+1))
		done
		$ECHO
		$ECHO -n "         Continue with MPP database startup yY|nN >"
		read REPLY
		if [ -z $REPLY ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
}	

PROCESS_STARTUP () {
		INDEX=0
		QE_START_COUNT=0
		for QNODE in "${QE_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $QNODE|$CUT -d"|" -f1`
				QE_DIR=`$ECHO $QNODE|$CUT -d"|" -f2`
				QE_PORT=`$ECHO $QNODE|$CUT -d"|" -f3`
				##MPP	  TAIL_VALID=`$ECHO $TNODE|$CUT -d"|" -f4`
				LOG_MSG "[INFO]:-Starting QE $QE_NAME directory $QE_DIR"
				##MPP	  if [ $TAIL_VALID != "t" ]; then
				##MPP	  	LOG_MSG "[WARN]:-Host $TAIL_NAME segment $TAIL_DIR marked as invalid, will not try to start"
				##MPP		((FAILED_COUNT=$FAILED_COUNT+1))
				##MPP	  else
				##CC	  	$TRUSTED_SHELL $QE_NAME ". $ENV_SETUP;export PGPORT=${QE_PORT};$PG_CTL $WAIT -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT} $MODE" >/dev/null 2>&1
				$TRUSTED_SHELL $QE_NAME "export PGPORT=${QE_PORT};$PG_CTL $WAIT -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${QE_PORT}\" start" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						((FAILED_COUNT=$FAILED_COUNT+1))
						LOG_MSG "[WARN]:-Startup request returned error code $RETVAL"
						LOG_MSG "[INFO]:-Tail'ing QE instance log file"
						$TRUSTED_SHELL $QE_NAME "$LS $QE_DIR" > /dev/null 2>&1
						if [ $? -ne 0 ]; then
								LOG_MSG "[WARN]:-QE directory $QE_DIR on $QE_NAME does not exist"
						else
								$TRUSTED_SHELL $QE_NAME "$TAIL ${QE_DIR}.log"|$TEE -a $LOG_FILE
								LOG_MSG "[WARN]:-Issue with $TRUSTED_SHELL to $QE_NAME for $QE_DIR database"
								EXIT_STATUS=1
						fi
						#MPP			if [ $FAILED ]; then
						#MPP				LOG_MSG "[INFO]:-Adding $TAIL_NAME to failed list"
						#MPP				FAILED_LIST_ARRAY[$INDEX]=${TAIL_NAME}:${TAIL_DIR}:${TAIL_PORT}
						#MPP			else
						#MPP				LOG_MSG "[WARN]:-Option -x not supplied, will not add $TAIL_NAME to failed list"	
						#MPP			fi
				else
						LOG_MSG "[INFO]:-Successfully started QE $QE_NAME database"
				fi
				#Now check status of postmaster on the tail
				LOG_MSG "[INFO]:-Checking status via pg_ctl"
				RET_TEXT=`$TRUSTED_SHELL $QE_NAME "$PG_CTL status -D $QE_DIR"`
				RUNNING=`$ECHO $RET_TEXT|$GREP -c "not running"`
				if [ $RUNNING -ne 0 ]; then
						LOG_MSG "[WARN]:-Command pg_ctl reports $QE_NAME for directory $QE_DIR port $QE_PORT not running"
						EXIT_STATUS=1
				fi
				if [ $RUNNING -eq 0 ] && [ $RETVAL -ne 0 ]; then
						LOG_MSG "[WARN]:-Could not start instance, BUT pg_ctl reports instance active, check instance log file via mppstate.bsh -e"
				fi
				if [ $RUNNING -eq 0 ] && [ $RETVAL -eq 0 ]; then
						LOG_MSG "[INFO]:-Command pg_ctl indicates started QE $QE_NAME database"
				fi
				((QE_START_COUNT=$QE_START_COUNT+1))
				##MPP		fi
				((INDEX=$INDEX+1))
		done
		LOG_MSG "[INFO]:-Starting Master $QD_NAME directory $MASTER_DATA_DIRECTORY"
		if [ $WAIT == "-W" ]; then
				$SLEEP 2
		fi
		export PGPORT=${MASTER_PORT}
		$PG_CTL $WAIT  -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT " start -m $MODE  >/dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Issue with startup of Master database" 2
		else
				LOG_MSG "[INFO]:-Successfully started Master $QD_NAME database"
		fi
		if [ $FAILED_COUNT -ne 0 ]; then
				LOG_MSG "[WARN]:-Failed to start $FAILED_COUNT of a total of $QE_COUNT QE instances"
		else
				LOG_MSG "[INFO]:----------------------------------------------------------"
				LOG_MSG "[INFO]:-Successfully started $QE_START_COUNT QE instances and Master instance"
		fi
}

MARK_INVALID () {
		LOG_MSG "[INFO]:-Processing list of failed segments"
		for TARGET in "${FAILED_LIST_ARRAY[@]}"
				do
				QE_NAME=`$ECHO $TARGET|$CUT -d: -f1`
				QE_DIR=`$ECHO $TARGET|$CUT -d: -f2`
				QE_PORT=`$ECHO $TARGET|$CUT -d: -f3`
				#MPP	$PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"update mpp_instance set valid='f' where host='$TAIL_NAME' and datadir='$TAIL_DIR' and port=$TAIL_PORT ;" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						LOG_MSG "[WARN]:-Issue with marking $QE_NAME as invalid, check log file"
						EXIT_STATUS=1
				else
						LOG_MSG "[INFO]:-Marked $QE_NAME as invalid, for data directory $QE_DIR, port $QE_PORT"
				fi
		done
}

#******************************************************************************
# Main Section
#******************************************************************************
if [ $# -ne 0 ]; then
		while getopts ":vt'?'axqnml:d:" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						a ) unset INTERACTIVE ;;
						x ) FAILED=1 ;;
						q ) unset VERBOSE ;;
						n ) WAIT="-W" ;; 
						m ) ADMIN=1 ;;
						l ) LOG_DIR=$OPTARG ;;
						* ) USAGE ;;
				esac
		done	
fi
if [ $FAILED ] && [ $WAIT == "-W" ]
		then
		ERROR_EXIT "[FATAL]:-Cannot use -x and -n options together" 2
fi

LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
CHK_QD_DB_RUNNING
CHK_DB_QD_SET
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
if [ $ADMIN ]; then
		START_ADMIN
		exit
fi
GET_DETAILS
if [ $INTERACTIVE ]; then
		PROMPT_CONTINUE
fi
PROCESS_STARTUP
if [ ${#FAILED_LIST_ARRAY[@]} -ne 0 ]; then
		MARK_INVALID
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
