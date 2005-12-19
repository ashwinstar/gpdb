#!/bin/bash
#	Filename:-		mppstate.sh
#	Version:-		$Revision$	
#	Updated:-		$Date$
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
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Reports on the state of the MPP database"
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
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master database directory> name of alternative Master database to report status of [optional]"
		$ECHO
		$ECHO "      Status options:"
		$ECHO "      -s, Display MPP database status [default]"
		$ECHO "      -m, List mirror segments of MPP database"
		$ECHO "      -c, List primary/mirror segment instance mapping of MPP database"
		$ECHO "      -p, List MPP database ports"
		$ECHO "      -n, List instance log file names"
		$ECHO "      -e, Display end of each segment instance log file"
		$ECHO "      -b, Display a brief MPP cluster status summary"
		$ECHO "      -i, Display system version information"
		$ECHO "      -f, Display details of fail-over master host if configured"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated"
		$ECHO "      2 Fatal error, unable to complete requested operation"
		$ECHO
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
		$ECHO "--------------------------------"
		$ECHO "MPP Master database port      = $MASTER_PORT"
		$ECHO "MPP Master database directory = $MASTER_DATA_DIRECTORY [Override with -d <head directory>]"
		if [ $VERBOSE ]; then
				$ECHO "Verbose                       = True             [Override with -q]"
		else
				$ECHO "Verbose                       = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}

CHK_PG_PID () {
		OS_PID_COUNT=`$PS ${PS_TXT}|$GREP $1|$GREP -v grep|$WC -l`
		if [ $OS_PID_COUNT = 0 ]; then
			ERROR_EXIT "[FATAL]:-Have postmaster.pid, but no process with PID = $1, check postmaster log file" 2
		fi

}

GET_FAIL_SETTING () {
	FAIL_TYPE=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"show mpp_fault_action;"`
	ERROR_CHK $? "obtain fault action setting" 2
}

GET_STANDBY_SETTING () {
	MASTER_STANDBY_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"select hostname from mpp_configuration where content=-1 and dbid<>1;"`
	ERROR_CHK $? "Obtain standby hostname" 1
	if [ x"" == x"$MASTER_STANDBY_HOSTNAME" ];then
		STANDBY_CONFIGURED="No master standby configured"
	else
		STANDBY_CONFIGURED="$MASTER_STANDBY_HOSTNAME"
		if [ `$HOSTNAME` == $MASTER_STANDBY_HOSTNAME ];then
			STANDBY_STATE="STANDBY HOST ACTIVE QD MASTER"
		else
			STANDBY_STATE="Standby host passive";fi
	fi
}

DATABASE_STATUS () {
		#MPP converted 21/07/2005
		QD_STATE_COUNT=0
		QD_FAILED_COUNT=0
		QD_VALID_COUNT=0
		QD_NOTVALID_COUNT=0
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Master Dispatch database down" 1
		else
				QD_PID=`$HEAD -1 $MASTER_DATA_DIRECTORY/$PG_PID`
				CHK_PG_PID $QD_PID
				GET_QE_DETAILS
				GET_MPP_VERSION
				GET_FAIL_SETTING
				GET_STANDBY_SETTING
				QD_PG_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"show server_version;"`
				ERROR_CHK $? "obtain postgres version" 1
				MIRROR_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid)/count(distinct(content)) from mpp_configuration where content<>-1;" 2> /dev/null `
				ERROR_CHK $? "obtain mirror count" 1
				if [ x"" == x"$MIRROR_COUNT" ];then
					MIRROR_CONFIG=ERROR
				else
					if [ $MIRROR_COUNT -ne 2 ]; then
						MIRROR_CONFIG=Off
					else
						MIRROR_CONFIG=on
					fi
				fi
				LOG_MSG "[INFO]:-Master Configuration & Status"
				LOG_MSG "[INFO]:--------------------------"
				LOG_MSG "[INFO]:-Master host                   = $QD_NAME" 
				LOG_MSG "[INFO]:-Master Postmaster process PID = $QD_PID" 
				LOG_MSG "[INFO]:-Database name                 = $QD_DBNAME "
				LOG_MSG "[INFO]:-Master port                   = $MASTER_PORT"
				LOG_MSG "[INFO]:-MPP software version          = $MPP_VERSION"
				LOG_MSG "[INFO]:-Postgres version              = $QD_PG_VERSION"
				LOG_MSG "[INFO]:-MPP failure action            = $FAIL_TYPE"
				LOG_MSG "[INFO]:-MPP mirroring status          = $MIRROR_CONFIG"
				LOG_MSG "[INFO]:-MPP master standby            = $STANDBY_CONFIGURED"
				LOG_MSG "[INFO}:-MPP master standby state      = $STANDBY_STATE"
				if [ -f $RECOVER_FLAG ]; then
						LOG_MSG "[WARN]:-Active mpprecovernode.bsh     = True"
				else
						LOG_MSG "[INFO]:-Active mpprecovernode.bsh     = False"
				fi
				LOG_MSG "[INFO]:-Segment instance cluster status"
				if [ ${#QE_ARRAY[@]} -eq 0 ]; then
						LOG_MSG "[WARN]:-Could not obtain any segment instance host details"
				else
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
								QE_PID=`$TRUSTED_SHELL $QE_NAME "if [ -f $QE_DIR/$PG_PID ]; then $HEAD -1 $QE_DIR/$PG_PID;else echo 0; fi"` 2>> $LOG_FILE
						RETVAL=$?
						if [ $RETVAL -ne 0 ]; then
								LOG_MSG "[WARN]:-Request for PID for $QE_NAME returned error code $RETVAL"
								QE_PID=0
						fi
						GET_PG_PID_ACTIVE $QE_PORT $QE_NAME
						QE_PID_ACTUAL=$PID
						LOG_MSG "[INFO]:-Segment Instance Physical PID for $QE_NAME port $QE_PORT       = $QE_PID_ACTUAL"
						STATUS=`$TRUSTED_SHELL $QE_NAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;export PGPORT=${QE_PORT};$PG_CTL status -D $QE_DIR"|$HEAD -1|$GREP -c PID` 2>> $LOG_FILE
						RETVAL=$?
						if [ $RETVAL -ne 0 ]; then
								LOG_MSG "[WARN]:-pg_ctl status from $QE_NAME port = $QE_PORT returned error code $RETVAL"
								((QD_FAILED_COUNT=$QD_FAILED_COUNT+1))	
								STATUS=0
						fi
						if [ $QE_PID -eq 0 ]; then
								LOG_MSG "[WARN]:-Segment instance $TYPE $QE_NAME port $QE_PORT postmaster PID = NOT RUNNING"
								EXIT_STATUS=1
						else
								LOG_MSG "[INFO]:-Segment instance $TYPE $QE_NAME port $QE_PORT postmaster PID = $QE_PID" 
						fi
						if [ $STATUS -eq 1 ]; then
								LOG_MSG "[INFO]:-Segment instance $TYPE $QE_NAME port $QE_PORT pg_ctl status  = Active"
								((QD_STATE_COUNT=$QD_STATE_COUNT+1))
						else
								LOG_MSG "[WARN]:-Segment instance $TYPE $QE_NAME port $QE_PORT pg_ctl status  = Failed"
								EXIT_STATUS=1
						fi
						if [ "$QE_VALID" == "t" ]; then
							((QD_VALID_COUNT=$QD_VALID_COUNT+1))
							LOG_MSG "[INFO]:-Segment instance $TYPE $QE_NAME port $QE_PORT segment status = Valid"
						else
							((QD_NOTVALID_COUNT=$QD_NOTVALID_COUNT+1))
							LOG_MSG "[WARN]:-Segment instance $TYPE $QE_NAME port $QE_PORT segment status = Invalid"
							EXIT_STATUS=1
						fi
						$ECHO
				done
		fi
		LOG_MSG "[INFO]:-Segment instance Instance Status Report"
		LOG_MSG "[INFO]:----------------------------------------"
		LOG_MSG "[INFO]:-Segment instance's reported active via pg_ctl = $QD_STATE_COUNT "
		LOG_MSG "[INFO]:-Segment instance's reported failed via pg_ctl = $QD_FAILED_COUNT"
		LOG_MSG "[INFO]:-Segment instance's marked as vaild            = $QD_VALID_COUNT"
		LOG_MSG "[INFO]:-Segment instance's marked as invalid          = $QD_NOTVALID_COUNT"
fi
}

VERSION_INFO () {
	GET_QE_DETAILS
	MASTER_POSTGRES_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $DEFAULTDB -c"show server_version;"`
	ERROR_CHK $? "obtain postgres server version" 1
	LOG_MSG "[INFO]:-Master database postgres version = $MASTER_POSTGRES_VERSION"
	for TNODE in "${QE_ARRAY[@]}"
	do
		SET_VAR
		POSTGRES_VERSION=`$TRUSTED_SHELL $QE_NAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -A -t -p $QE_PORT $DEFAULTDB -c\"show server_version;\""`
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
			LOG_MSG "[WARN]:-Unable to connect to $QE_NAME on port $QE_PORT"
			ERROR_EXIT=1
		else
			LOG_MSG "[INFO]:-$QE_NAME segment directory $QE_DIR postgres version = $POSTGRES_VERSION"
		fi
	done

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
						if [ "$QE_PRIMARY" == "t" ] && [ "$QE_VALID" == 't' ];then
							STATUS="Active"
						else
							if [ "$QE_PRIMARY" == "f" ] && [ "$QE_VALID" == 't' ]; then
								STATUS="Passive"
							else
								STATUS="Failed"
							fi
						fi
						$TABECHO "[INFO]:-Mirror  = $QE_NAME data directory = $QE_DIR port = $QE_PORT status = $STATUS"
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
		QE_MIRROR_FAILED=0
		QE_COUNT=0
		if [ ! -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[WARN]:-Master database down" 1
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
		((QE_COUNT=$QE_COUNT+1))
done
for TNODE in "${QE_MIRROR_ARRAY[@]}"
		do
		SET_VAR
		if [ "$QE_PRIMARY" == "t" ] && [ "$QE_VALID" == 't' ]; then
			((QE_MIRROR_ACTIVE=$QE_MIRROR_ACTIVE+1))
		else
			if [ "$QE_PRIMARY" == "f" ] && [ "$QE_VALID" == 't' ]; then
				((QE_MIRROR_PASSIVE=$QE_MIRROR_PASSIVE+1))
			else
				((QE_MIRROR_FAILED=$QE_MIRROR_FAILED+1))
			fi
		fi
		QE_PID=`$TRUSTED_SHELL $QE_NAME "if [ -f $QE_DIR/$PG_PID ]; then $HEAD -1 $QE_DIR/$PG_PID;else echo 0
fi"` 2>> $LOG_FILE
RETVAL=$?
if [ $RETVAL -ne 0 ]; then
		((QE_MIRROR_FAILED_COUNT=$QE_MIRROR_FAILED_COUNT+1))
else
		((QE_MIRROR_STATE_COUNT=$QE_MIRROR_STATE_COUNT+1))
fi
((QE_COUNT=$QE_COUNT+1))
		done
		LOG_MSG "[INFO]:-MPP cluster status summary"
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-Master db $QD_DBNAME active" 
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-Total segment instance MPP count              = $QE_COUNT"
		LOG_MSG "[INFO]:-Total segment instance primary PID active     = $QE_PRIMARY_STATE_COUNT"
		if [ $QE_PRIMARY_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total segment instance primary PID failed     = $QE_PRIMARY_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total segment instance primary PID failed     = $QE_PRIMARY_FAILED_COUNT"
				EXIT_STATUS=1
		fi	
		LOG_MSG "[INFO]:-Total segment instance primary db active      = $QE_PRIMARY_VALID"
		if [ $QE_PRIMARY_INVALID -eq 0 ]; then
			LOG_MSG "[INFO]:-Total segment instance primary db failed      = $QE_PRIMARY_INVALID"
		else
			LOG_MSG "[WARN]:-Total segment instance primary db failed      = $QE_PRIMARY_INVALID"
			EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-Total segment instance mirror PID active      = $QE_MIRROR_STATE_COUNT"
		if [ $QE_MIRROR_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total segment instance mirror PID failed      = $QE_MIRROR_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total segment instance mirror PID failed      = $QE_MIRROR_FAILED_COUNT"
				EXIT_STATUS=1
		fi
		if [ $QE_MIRROR_ACTIVE -ne 0 ];then
			LOG_MSG "[WARN]:-Total segment instance mirror active primary  = $QE_MIRROR_ACTIVE"
			EXIT_STATUS=1
		else
			LOG_MSG "[INFO]:-Total segment instance mirror active primary  = $QE_MIRROR_ACTIVE"
		fi
		if [ $QE_MIRROR_FAILED -ne 0 ];then
			LOG_MSG "[WARN]:-Total segment instance mirror failed          = $QE_MIRROR_FAILED"
		else
			LOG_MSG "[INFO]:-Total segment instance mirror failed          = $QE_MIRROR_FAILED"
		fi
		LOG_MSG "[INFO]:-Total segment instance mirror passive primary = $QE_MIRROR_PASSIVE"
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
		PORT_ARRAY=(`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname, port, datadir from mpp_configuration where content=-1 and dbid=1;" `)
		ERROR_CHK $? "obtain port assignments" 2
		LOG_MSG "[INFO]:-Port assignments"
		for PMODE in "${PORT_ARRAY[@]}"
				do
				HOST=`$ECHO $PMODE|$CUT -d"|" -f1`
				PORT=`$ECHO $PMODE|$CUT -d"|" -f2`
				DIR=`$ECHO $PMODE|$CUT -d"|" -f3`
				LOG_MSG "[INFO]:-Segment instance $HOST $DIR  port = $PORT"
		done
}

LIST_MPP_LOGS () {
		LOG_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select hostname, datadir from mpp_configuration where content=-1 ;" 2>|/dev/null`)
		ERROR_CHK $? "obtain log assignments" 2
		LOG_MSG "[INFO]:-Segment instance MPP database instance log file names"
		for LOGCHK in "${LOG_ARRAY[@]}"
				do
				HOST=`$ECHO $LOGCHK|$CUT -d"|" -f1`
				DIR=`$ECHO $LOGCHK|$CUT -d"|" -f2`
				if [ $TAIL_LOGS ]; then
						LOG_MSG "[INFO]:-$HOST $DIR log file tail START---------------"
						$ECHO
						$TRUSTED_SHELL $HOST "$TAIL ${DIR}.log"|$TEE -a $LOG_FILE
						$ECHO
						LOG_MSG "[INFO]:-$HOST $DIR log file tail END-----------------"
				else
						LOG_MSG "[INFO]:-$HOST $DIR has logfile ${DIR}.log database logfile"
				fi
		done	
}

STANDBY_HOST_DETAILS () {
	GET_STANDBY_DETAILS
	if [ ${#STANDBY_ARRAY[@]} -eq 0 ];then
		LOG_MSG "[INFO]:-Standby master instance not configured"
	else
		MASTER_STANDBY_HOSTNAME=`$ECHO $STANDBY_ARRAY|$CUT -d"|" -f1`
		MASTER_STANDBY_DIR=`$ECHO $STANDBY_ARRAY|$CUT -d"|" -f2`
		MASTER_STANDBY_PORT=`$ECHO $STANDBY_ARRAY|$CUT -d"|" -f3`
		if [ `$HOSTNAME` == $MASTER_STANDBY_HOSTNAME ];then
			STANDBY_STATE="STANDBY HOST ACTIVE QD MASTER"
		else
			STANDBY_STATE="Standby host passive";fi
		GET_PG_PID_ACTIVE $MASTER_STANDBY_PORT $MASTER_STANDBY_HOSTNAME
		LOG_MSG "[INFO]:-Standby master details"
		LOG_MSG "[INFO]:-----------------------"
		LOG_MSG "[INFO]:-Standby hostname        = $MASTER_STANDBY_HOSTNAME"
		LOG_MSG "[INFO]:-Standby data directory  = $MASTER_STANDBY_DIR"
		LOG_MSG "[INFO]:-Standby port            = $MASTER_STANDBY_PORT"
		LOG_MSG "[INFO]:-Standby PID             = $PID"
		LOG_MSG "[INFO]:-Standby status          = $STANDBY_STATE"
	fi
}

GET_MPP_VERSION () {
		MPP_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select * from mpp_version_at_initdb limit 1;" 2>/dev/null`
		ERROR_CHK $? "obtain MPP version information" 2
}

NOT_ACTIVE () {
		LOG_MSG "[WARN]:-This fucntion not supported at this MPP release, see -? for help"
}

#******************************************************************************
# Main Section
#******************************************************************************
#Check to see if we have a parameter supplied to script
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
SELECT=1
if [ $# -ne 0 ]; then
		while getopts ":vtbl:d:nieqsmcpf'?'" opt;do
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
						i ) SELECT=7 ;;
						f ) SELECT=8 ;;
						e ) TAIL_LOGS=1 
						    SELECT=5 ;;
						l ) LOG_DIR=$OPTARG ;;
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
CHK_ON_PASSIVE_STANDBY
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
		7 ) VERSION_INFO ;;
		8 ) STANDBY_HOST_DETAILS ;;
esac
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
