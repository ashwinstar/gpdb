#!/bin/bash
#	Filename:-		mppstate.sh
#	Version:-		1.1
#	Updated:-		29/07/2005
#	Status:-		Conversion to MPP
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Provides state of the cluster database.
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
# 1.1	29/07/2005	G Coombe	Added a brief status output option.
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
VERBOSE=1
unset TAIL_LOGS
PROG_NAME=`$BASENAME $0`
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
QD_NAME=`hostname`
EXIT_STATUS=0
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$TABECHO
		$TABECHO $0
		$TABECHO "Reports on the state of the MPP database"
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
		$TABECHO
		$TABECHO "Connection options:"
		$TABECHO "-d, <QD database directory> name of alternative QD database to report status of [optional]"
		$TABECHO
		$TABECHO "Status options:"
		$TABECHO "-s, Display MPP database status [default]"
		$TABECHO "-m, List mirror segments of MPP database"
		$TABECHO "-c, List primary/mirror segment mapping of MPP database"
		$TABECHO "-p, List MPP database ports"
		$TABECHO "-n, List instance log file names"
		$TABECHO "-e, Display end of each QE log file"
		$TABECHO "-b, Display a brief MPP cluster status summary"
		$TABECHO
		$TABECHO "Return codes:"
		$TABECHO "0 No problems encountered with requested operation"
		$TABECHO "1 Warning generated"
		$TABECHO "2 Fatal error, unable to complete requested operation"
		$TABECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

DISPLAY_PARAM () {
		GET_QD_PORT $QD_DATA_DIRECTORY
		$ECHO
		$ECHO "Current Default State Parameters"
		$ECHO "-----------------------------------"
		$ECHO "MPP QD database port      =$QD_PORT"
		$ECHO "MPP QD database directory = $QD_DATA_DIRECTORY [Override with -d <head directory>]"
		if [ $VERBOSE ]; then
				$ECHO "Verbose                   = True            [Override with -q]"
		else
				$ECHO "Verbose                   = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}


DATABASE_STATUS () {
		#MPP converted 21/07/2005
		QD_STATE_COUNT=0
		QD_FAILED_COUNT=0
		if [ ! -s $QD_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Query Dispatch database down" 1
		else
				GET_QE_DETAILS
				GET_MPP_VERSION	
				QD_PID=`$HEAD -1 $QD_DATA_DIRECTORY/$PG_PID`
				LOG_MSG "[INFO]:-QD Configuration & Status"
				LOG_MSG "[INFO]:--------------------------"
				LOG_MSG "[INFO]:-QD host = $QD_NAME" 
				LOG_MSG "[INFO]:-QD Postmaster process PID = $QD_PID" 
				LOG_MSG "[INFO]:-Database name = $QD_DBNAME "
				LOG_MSG "[INFO]:-QD port = $QD_PORT"
				LOG_MSG "[INFO]:-MPP software version = $MPP_VERSION"
				if [ -f $RECOVER_FLAG ]; then
						LOG_MSG "[WARN]:-Active mpprecovernode.bsh = True"
				else
						LOG_MSG "[INFO]:-Active mpprecovernode.bsh = False"
				fi
				LOG_MSG "[INFO]:-QE cluster node status"
				if [ ${#QE_ARRAY[@]} -eq 0 ]; then
						LOG_MSG "[WARN]:-Could not obtain any QE host details"
				else
						LOG_MSG "[INFO]:-QE PID & pg_ctl Status"
						LOG_MSG "[INFO]:-----------------------"
						for TNODE in "${QE_ARRAY[@]}"
								do
								QE_NAME=`$ECHO $TNODE|$CUT -d"|" -f1`
								QE_DIR=`$ECHO $TNODE|$CUT -d"|" -f2`
								QE_PORT=`$ECHO $TNODE|$CUT -d"|" -f3`
								QE_PID=`$TRUSTED_SHELL $QE_NAME "if [ -f $QE_DIR/$PG_PID ]; then $HEAD -1 $QE_DIR/$PG_PID;else echo 0
						fi"` 2>> $LOG_FILE
						RETVAL=$?
						if [ $RETVAL -ne 0 ]; then
								LOG_MSG "[WARN]:-Request for PID for $QE_NAME returned error code $RETVAL"
								QE_PID=0
						fi
						STATUS=`$TRUSTED_SHELL $QE_NAME "export PGPORT=${QE_PORT};$PG_CTL status -D $QE_DIR"|$HEAD -1|$GREP -c PID` 2>> $LOG_FILE
						RETVAL=$?
						if [ $RETVAL -ne 0 ]; then
								LOG_MSG "[WARN]:-Request for pg_ctl status from $QE_NAME returned error code $RETVAL"
								((QD_FAILED_COUNT=$QD_FAILED_COUNT+1))	
								STATUS=0
						fi
						if [ $QE_PID -eq 0 ]; then
								LOG_MSG "[WARN]:-QE host $QE_NAME port $QE_PORT postmaster PID = NOT RUNNING"
								EXIT_STATUS=1
						else
								LOG_MSG "[INFO]:-QE host $QE_NAME port $QE_PORT postmaster PID = $QE_PID" 
						fi
						if [ $STATUS -eq 1 ]; then
								LOG_MSG "[INFO]:-QE host $QE_NAME pg_ctl reports status = Active"
								((QD_STATE_COUNT=$QD_STATE_COUNT+1))
						else
								LOG_MSG "[WARN]:-QE host $QE_NAME pg_ctl reports status = Failed"
								EXIT_STATUS=1
						fi
				done
		fi
		LOG_MSG "[INFO]:---------------------------------------------------"
		LOG_MSG "[INFO]:-State reports $QD_STATE_COUNT QE's active $QD_FAILED_COUNT Failed and QD active"
fi
}

MIRROR_LIST () {
		GET_QE_MIRROR_DETAILS
		LOG_MSG "[INFO]:-Mirror list"
		if [ ${#QE_MIRROR_ARRAY[@]} -ne 0 ]; then
				for MNODE in "${QE_MIRROR_ARRAY[@]}"
						do
						QE_NAME=`$ECHO $MNODE|$CUT -d"|" -f1`
						QE_DIR=`$ECHO $MNODE|$CUT -d"|" -f2`
						QE_PORT=`$ECHO $MNODE|$CUT -d"|" -f3`
						$TABECHO "[INFO]:-Mirror QE = $QE_NAME data directory = $QE_DIR port = $QE_PORT"
						EXIT_STATUS=0
				done
		else
				LOG_MSG "[WARN]:-No mirrors defined"
				EXIT_STATUS=1
		fi
}

SET_VAR () {
		QE_NAME=`$ECHO $TNODE|$CUT -d"|" -f1`
		QE_DIR=`$ECHO $TNODE|$CUT -d"|" -f2`
		QE_PORT=`$ECHO $TNODE|$CUT -d"|" -f3`
}

SUMMARY_STATUS () {
		QE_PRIMARY_STATE_COUNT=0
		QE_PRIMARY_FAILED_COUNT=0
		QE_MIRROR_STATE_COUNT=0
		QE_MIRROR_FAILED_COUNT=0
		QE_COUNT=0
		if [ ! -s $QD_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Query Dispatch database down" 1
		else
				GET_QE_PRIMARY_DETAILS
				GET_QE_MIRROR_DETAILS
		fi
		LOG_MSG "[INFO]:-Processing status request, please wait"
		for TNODE in "${QE_PRIMARY_ARRAY[@]}"
				do
				SET_VAR
				QE_PID=`$TRUSTED_SHELL $QE_NAME "if [ -f $QE_DIR/$PG_PID ]; then $HEAD -1 $QE_DIR/$PG_PID;else echo 0
		fi"` 2>> $LOG_FILE
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				((QE_PRIMARY_FAILED_COUNT=$QE_PRIMARY_FAILED_COUNT+1))
		else
				((QE_PRIMARY_STATE_COUNT=$QE_PRIMARY_STATE_COUNT+1))
		fi
		$ECHO -n .
		((QE_COUNT=$QE_COUNT+1))
done
for TNODE in "${QE_MIRROR_ARRAY[@]}"
		do
		SET_VAR
		QE_PID=`$TRUSTED_SHELL $QE_NAME "if [ -f $QE_DIR/$PG_PID ]; then $HEAD -1 $QE_DIR/$PG_PID;else echo 0
fi"` 2>> $LOG_FILE
RETVAL=$?
if [ $RETVAL -ne 0 ]; then
		((QE_MIRROR_FAILED_COUNT=$QE_MIRROR_FAILED_COUNT+1))
else
		((QE_MIRROR_STATE_COUNT=$QE_MIRROR_STATE_COUNT+1))
fi
$ECHO -n .
((QE_COUNT=$QE_COUNT+1))
		done
		$ECHO
		LOG_MSG "[INFO]:-MPP cluster status summary"
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-QD db $QD_DBNAME active" 
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-Total QE MPP count      = $QE_COUNT"
		LOG_MSG "[INFO]:-Total QE primary active = $QE_PRIMARY_STATE_COUNT"
		if [ $QE_PRIMARY_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total QE primary failed = $QE_PRIMARY_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total QE primary failed = $QE_PRIMARY_FAILED_COUNT"
		fi	
		LOG_MSG "[INFO]:-Total QE mirror active  = $QE_MIRROR_STATE_COUNT"
		if [ $QE_MIRROR_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total QE mirror failed  = $QE_MIRROR_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total QE mirror failed  = $QE_MIRROR_FAILED_COUNT"
		fi
}

CLUSTER_CONFIG () {
		GET_QE_PRIMARY_DETAILS
		GET_QE_MIRROR_DETAILS
		if [ ${#QE_MIRROR_ARRAY[@]} -ne 0 ]; then
				LOG_MSG "[INFO]:-Primary to mirror mapping "
				ARRAY_MAX=${#QE_MIRROR_ARRAY[@]}
				COUNT=0
				while [ $COUNT -lt $ARRAY_MAX ]
						do
						PHOST=`$ECHO ${QE_PRIMARY_ARRAY[$COUNT]}|$CUT -d"|" -f1`
						PDIR=`$ECHO ${QE_PRIMARY_ARRAY[$COUNT]}|$CUT -d"|" -f2`
						PPORT=`$ECHO ${QE_PRIMARY_ARRAY[$COUNT]}|$CUT -d"|" -f3`
						MHOST=`$ECHO ${QE_MIRROR_ARRAY[$COUNT]}|$CUT -d"|" -f1`
						MDIR=`$ECHO ${QE_MIRROR_ARRAY[$COUNT]}|$CUT -d"|" -f2`
						MPORT=`$ECHO ${QE_MIRROR_ARRAY[$COUNT]}|$CUT -d"|" -f3`
						$TABECHO "[INFO]:-Primary $PHOST  $PDIR  $PPORT  mirror $MHOST  $MDIR  $MPORT"
						((COUNT=$COUNT+1))
				done

		else
				LOG_MSG "[INFO]:-Primary list [Mirroring not implemented]"
				for PNODE in "${QE_PRIMARY_ARRAY[@]}"
						do
						PHOST=`$ECHO $PNODE|$CUT -d "|" -f1`
						PDIR=`$ECHO $PNODE|$CUT -d "|" -f2`
						PPORT=`$ECHO $PNODE|$CUT -d "|" -f3`
						$TABECHO "[INFO]:-Primary $PHOST  $PDIR  $PPORT"
				done
		fi
}

LIST_MPP_PORTS () {
		PORT_ARRAY=(`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c"select b.hostname, a.port from cdb.listener a, cdb.host b where listenerid<> (select listenerid from cdb.pgdatabase where content=-1) and a.hostid=b.hostid;" 2>|/dev/null`)
		LOG_MSG "[INFO]:-Port assignments"
		for PMODE in "${PORT_ARRAY[@]}"
				do
				HOST=`$ECHO $PMODE|$CUT -d"|" -f1`
				PORT=`$ECHO $PMODE|$CUT -d"|" -f2`
				LOG_MSG "[INFO]:-QE host $HOST has assigned MPP port = $PORT"
		done
}

LIST_MPP_LOGS () {
		LOG_ARRAY=(`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c "select b.hostname,  a.datadir from cdb.listener a, cdb.host b where listenerid<> (select listenerid from cdb.pgdatabase where content=-1) and a.hostid=b.hostid;" 2>|/dev/null`)
		LOG_MSG "[INFO]:-QE MPP database instance log file names"
		for LOGCHK in "${LOG_ARRAY[@]}"
				do
				HOST=`$ECHO $LOGCHK|$CUT -d"|" -f1`
				DIR=`$ECHO $LOGCHK|$CUT -d"|" -f2`
				if [ $TAIL_LOGS ]; then
						LOG_MSG "[INFO]:-QE host $HOST log file tail START---------------"
						$ECHO
						$TRUSTED_SHELL $HOST "$TAIL ${DIR}.log"|$TEE -a $LOG_FILE
						$ECHO
						LOG_MSG "[INFO]:-QE host $HOST log file tail END-----------------"
				else
						LOG_MSG "[INFO]:-QE Host assgined logfile ${DIR}.log database logfile"
				fi
		done	
}

GET_MPP_VERSION () {
		MPP_VERSION=`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c "select * from cdb.version" 2>/dev/null`
}

NOT_ACTIVE () {
		LOG_MSG "[WARN]:-This fucntion not supported at this MPP release, see -? for help"
}

#******************************************************************************
# Main Section
#******************************************************************************
#Check to see if we have a parameter supplied to script
SELECT=1
if [ $# -ne 0 ]; then
		while getopts ":vtbl:d:neqsmcp'?'" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						t ) DISPLAY_PARAM ;;
						'?' ) USAGE ;;
						d ) QD_DATA_DIRECTORY=$OPTARG ;;
						q ) unset VERBOSE ;;
						s ) ;;
						m ) SELECT=2 ;;
						c ) SELECT=3 ;;
						p ) SELECT=4 ;; 
						n ) SELECT=5 ;;
						b ) SELECT=6 ;;
						e ) TAIL_LOGS=1 
						SELECT=5;;
						l ) LOG_DIR=$OPTARG ;;
						* ) UNKNOWN_OPTION ;;
				esac
		done	
fi
CHK_DB_QD_SET
if [ $LOG_DIR ]
		then
		CHK_DIR $LOG_DIR
		if [ $EXISTS -eq 1 ]; then ERROR_EXIT "[FATAL]:-Log directory $LOG_DIR does not exist" 2
		fi
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
else
		LOG_FILE=$WORKDIR/logs/${PROG_NAME}_${CUR_DATE}.log
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
CHK_DB_RUNNING
GET_QD_PORT $QD_DATA_DIRECTORY
GET_QD_DB_NAME
case $SELECT in
		1 ) DATABASE_STATUS ;; 
		2 ) MIRROR_LIST ;;
		3 ) CLUSTER_CONFIG ;;
		4 ) LIST_MPP_PORTS ;;
		5 ) LIST_MPP_LOGS ;;
		6 ) SUMMARY_STATUS ;;
esac
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
