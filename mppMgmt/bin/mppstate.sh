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
		$TABECHO "-d, <Master database directory> name of alternative Master database to report status of [optional]"
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
		GET_MASTER_PORT $MASTER_DATA_DIRECTORY
		$ECHO
		$ECHO "Current Default State Parameters"
		$ECHO "-----------------------------------"
		$ECHO "MPP Master database port      =$MASTER_PORT"
		$ECHO "MPP Master database directory = $MASTER_DATA_DIRECTORY [Override with -d <head directory>]"
		if [ $VERBOSE ]; then
				$ECHO "Verbose                   = True            [Override with -q]"
		else
				$ECHO "Verbose                   = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}

CHK_PG_PID () {
		OS_PID_COUNT=`$PS ax|$GREP $1|$GREP -v grep|$WC -l`
		if [ $OS_PID_COUNT = 0 ]; then
			ERROR_EXIT "[FATAL]:-Have postmaster.pid, but no process with PID = $1, check postmaster log file" 2
		fi

}

GET_FAIL_SETTING () {
	FAIL_TYPE=`$PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"show MPPFaultAction;"`
}

DATABASE_STATUS () {
		#MPP converted 21/07/2005
		QD_STATE_COUNT=0
		QD_FAILED_COUNT=0
		QD_VALID_COUNT=0
		QD_NOTVALID_COUNT=0
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Query Dispatch database down" 1
		else
				QD_PID=`$HEAD -1 $MASTER_DATA_DIRECTORY/$PG_PID`
				CHK_PG_PID $QD_PID
				GET_QE_DETAILS
				GET_MPP_VERSION
				GET_FAIL_SETTING
				LOG_MSG "[INFO]:-Master Configuration & Status"
				LOG_MSG "[INFO]:--------------------------"
				LOG_MSG "[INFO]:-Master host                   = $QD_NAME" 
				LOG_MSG "[INFO]:-Master Postmaster process PID = $QD_PID" 
				LOG_MSG "[INFO]:-Database name                 = $QD_DBNAME "
				LOG_MSG "[INFO]:-Master port                   = $MASTER_PORT"
				LOG_MSG "[INFO]:-MPP software version          = $MPP_VERSION"
				LOG_MSG "[INFO]:-MPP failure action            = $FAIL_TYPE"
				if [ -f $RECOVER_FLAG ]; then
						LOG_MSG "[WARN]:-Active mpprecovernode.bsh     = True"
				else
						LOG_MSG "[INFO]:-Active mpprecovernode.bsh     = False"
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
								QE_VALID=`$ECHO $TNODE|$CUT -d"|" -f4`
								QE_PRIMARY=`$ECHO $TNODE|$CUT -d"|" -f5`
								if [ "$QE_PRIMARY" == "t" ];then
									TYPE="primary"
								else
									TYPE="mirror "
								fi
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
								LOG_MSG "[WARN]:-pg_ctl status from $QE_NAME port = $QE_PORT returned error code $RETVAL"
								((QD_FAILED_COUNT=$QD_FAILED_COUNT+1))	
								STATUS=0
						fi
						if [ $QE_PID -eq 0 ]; then
								LOG_MSG "[WARN]:-QE $TYPE $QE_NAME port $QE_PORT postmaster PID = NOT RUNNING"
								EXIT_STATUS=1
						else
								LOG_MSG "[INFO]:-QE $TYPE $QE_NAME port $QE_PORT postmaster PID = $QE_PID" 
						fi
						if [ $STATUS -eq 1 ]; then
								LOG_MSG "[INFO]:-QE $TYPE $QE_NAME port $QE_PORT pg_ctl status  = Active"
								((QD_STATE_COUNT=$QD_STATE_COUNT+1))
						else
								LOG_MSG "[WARN]:-QE $TYPE $QE_NAME port $QE_PORT pg_ctl status  = Failed"
								EXIT_STATUS=1
						fi
						if [ "$QE_VALID" == "t" ]; then
							((QD_VALID_COUNT=$QD_VALID_COUNT+1))
							LOG_MSG "[INFO]:-QE $TYPE $QE_NAME port $QE_PORT segment status = Valid"
						else
							((QD_NOTVALID_COUNT=$QD_NOTVALID_COUNT+1))
							LOG_MSG "[WARN]:-QE $TYPE $QE_NAME port $QE_PORT segment status = Invalid"
							EXIT_STATUS=1
						fi
						$ECHO
				done
		fi
		LOG_MSG "[INFO]:-QE Instance Status Report"
		LOG_MSG "[INFO]:--------------------------"
		LOG_MSG "[INFO]:-QE's reported active via pg_ctl = $QD_STATE_COUNT "
		LOG_MSG "[INFO]:-QE's reported failed via pg_ctl = $QD_FAILED_COUNT"
		LOG_MSG "[INFO]:-QE's marked as vaild            = $QD_VALID_COUNT"
		LOG_MSG "[INFO]:-QE's marked as invalid          = $QD_NOTVALID_COUNT"
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
						QE_VALID=`$ECHO $MNODE|$CUT -d"|" -f4`
						QE_PRIMARY=`$ECHO $MNODE|$CUT -d"|" -f5`
						if [ "$QE_PRIMARY" == "t" ];then
							STATUS="Active"
						else
							STATUS="Passive"
						fi
						$TABECHO "[INFO]:-Mirror QE = $QE_NAME data directory = $QE_DIR port = $QE_PORT status = $STATUS"
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
		QE_VALID=`$ECHO $TNODE|$CUT -d"|" -f4`
		QE_PRIMARY=`$ECHO $TNODE|$CUT -d"|" -f5`
}

SUMMARY_STATUS () {
		QE_PRIMARY_STATE_COUNT=0
		QE_PRIMARY_FAILED_COUNT=0
		QE_MIRROR_STATE_COUNT=0
		QE_MIRROR_FAILED_COUNT=0
		QE_PRIMARY_VALID=0
		QE_PRIMARY_INVALID=0
		QE_MIRROR_PASSIVE=0
		QE_MIRROR_ACTIVE=0
		QE_COUNT=0
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Query Dispatch database down" 1
		else
				GET_QE_PRIMARY_DETAILS
				GET_QE_MIRROR_DETAILS
		fi
		LOG_MSG "[INFO]:-Processing status request, please wait"
		for TNODE in "${QE_PRIMARY_ARRAY[@]}"
				do
				SET_VAR
				if [ "$QE_VALID" == "t" ]; then
					((QE_PRIMARY_VALID=$QE_PRIMARY_VALID+1))
				else
					((QE_PRIMARY_INVALID=$QE_PRIMARY_INVALID+1))
				fi
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
		if [ "$QE_PRIMARY" == "t" ]; then
			((QE_MIRROR_ACTIVE=$QE_MIRROR_ACTIVE+1))
		else
			((QE_MIRROR_PASSIVE=$QE_MIRROR_PASSIVE+1))
		fi
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
		LOG_MSG "[INFO]:-Master db $QD_DBNAME active" 
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-Total QE MPP count              = $QE_COUNT"
		LOG_MSG "[INFO]:-Total QE primary PID active     = $QE_PRIMARY_STATE_COUNT"
		if [ $QE_PRIMARY_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total QE primary PID failed     = $QE_PRIMARY_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total QE primary PID failed     = $QE_PRIMARY_FAILED_COUNT"
				EXIT_STATUS=1
		fi	
		LOG_MSG "[INFO]:-Total QE primary db active      = $QE_PRIMARY_VALID"
		if [ $QE_PRIMARY_INVALID -eq 0 ]; then
			LOG_MSG "[INFO]:-Total QE primary db failed      = $QE_PRIMARY_INVALID"
		else
			LOG_MSG "[WARN]:-Total QE primary db failed      = $QE_PRIMARY_INVALID"
			EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-Total QE mirror PID active      = $QE_MIRROR_STATE_COUNT"
		if [ $QE_MIRROR_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total QE mirror PID failed      = $QE_MIRROR_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total QE mirror PID failed      = $QE_MIRROR_FAILED_COUNT"
				EXIT_STATUS=1
		fi
		if [ $QE_MIRROR_ACTIVE -ne 0 ];then
			LOG_MSG "[WARN]:-Total QE mirror active primary  = $QE_MIRROR_ACTIVE"
			EXIT_STATUS=1
		else
			LOG_MSG "[INFO]:-Total QE mirror active primary  = $QE_MIRROR_ACTIVE"
		fi
		LOG_MSG "[INFO]:-Total QE mirror passive primary = $QE_MIRROR_PASSIVE"
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
		PORT_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select b.hostname, a.port from mpp.listener a, mpp.host b where listenerid<> (select listenerid from mpp.pgdatabase where content=-1) and a.hostid=b.hostid;" 2>|/dev/null`)
		LOG_MSG "[INFO]:-Port assignments"
		for PMODE in "${PORT_ARRAY[@]}"
				do
				HOST=`$ECHO $PMODE|$CUT -d"|" -f1`
				PORT=`$ECHO $PMODE|$CUT -d"|" -f2`
				LOG_MSG "[INFO]:-QE host $HOST has assigned MPP port = $PORT"
		done
}

LIST_MPP_LOGS () {
		LOG_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select b.hostname,  a.datadir from mpp.listener a, mpp.host b where listenerid<> (select listenerid from mpp.pgdatabase where content=-1) and a.hostid=b.hostid;" 2>|/dev/null`)
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
						LOG_MSG "[INFO]:-QE Host assigned logfile ${DIR}.log database logfile"
				fi
		done	
}

GET_MPP_VERSION () {
		MPP_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select * from mpp.version" 2>/dev/null`
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
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
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
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
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
