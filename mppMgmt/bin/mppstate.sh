#!/bin/bash
#	Filename:-		mppstate.sh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Provides state of the MPP instance.
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
TLINES=10
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Reports on the state of the MPP instance"
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
		$ECHO "      -d, <Master instance directory> name of alternative Master instance to report status of [optional]"
		$ECHO
		$ECHO "      Status options:"
		$ECHO "      -s, Display MPP instance status [default]"
		$ECHO "      -m, List mirror segments of MPP database"
		$ECHO "      -c, List primary/mirror segment instance mapping of MPP database"
		$ECHO "      -p, List MPP instance ports"
		$ECHO "      -n, List instance log file names"
		$ECHO "      -e, Display end of each segment instance log file"
		$ECHO "          Additional log display options"
		$ECHO "          -r, <number> Recall <number> of lines of log, with -e option"
		$ECHO "          default is to display $TLINES lines from log to screen"
		$ECHO "          -g, <string> Search for <string> in log, enclose space separated words within \" \""
		$ECHO "          To seach for multiple words, separate each with a pipe (|) symbol"
		$ECHO "      -b, Display a brief MPP instance status summary"
		$ECHO "      -i, Display system version information"
		$ECHO "      -f, Display details of standby master host if configured"
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
		$ECHO "MPP Master instance port      = $MASTER_PORT"
		$ECHO "MPP Master instance directory = $MASTER_DATA_DIRECTORY [Override with -d <master instance directory>]"
		if [ $VERBOSE ]; then
				$ECHO "Verbose                       = True             [Override with -q]"
		else
				$ECHO "Verbose                       = False"
		fi 
		$ECHO
		exit $EXIT_STATUS
}

GET_FAIL_SETTING () {
	FAIL_TYPE=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"show mpp_fault_action;"`
	ERROR_CHK $? "obtain fault action setting" 2
}

GET_STANDBY_SETTING () {
	MASTER_STANDBY_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"select hostname from $CONFIG_TABLE where content=-1 and dbid<>1;"`
	ERROR_CHK $? "Obtain standby hostname" 1
	if [ x"" == x"$MASTER_STANDBY_HOSTNAME" ];then
		STANDBY_CONFIGURED="No master standby configured"
	else
		STANDBY_CONFIGURED="$MASTER_STANDBY_HOSTNAME"
		if [ `$HOSTNAME` == $MASTER_STANDBY_HOSTNAME ];then
			STANDBY_STATE="STANDBY HOST ACTIVE INSTANCE MASTER"
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
				ERROR_EXIT "[WARN]:-Master instance down" 1
		else
				QD_PID=`$HEAD -1 $MASTER_DATA_DIRECTORY/$PG_PID`
				GET_PG_PID_ACTIVE $MASTER_PORT
				GET_QE_DETAILS
				GET_MPP_VERSION
				GET_FAIL_SETTING
				GET_STANDBY_SETTING
				QD_PG_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"show server_version;"`
				ERROR_CHK $? "obtain postgres version" 1
				QD_ROLE=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"show mpp_session_role;" 2> /dev/null`
				if [ $? -ne 0 ];then
				  QD_ROLE=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"show mpp_session_role;"`
				  ERROR_CHK $? "obtain master current role" 1
				fi
				MIRROR_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid)/count(distinct(content)) from $CONFIG_TABLE where content<>-1;" 2> /dev/null `
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
				LOG_MSG "[INFO]:-Master data directory         = $MASTER_DATA_DIRECTORY"
				LOG_MSG "[INFO]:-Database name                 = $QD_DBNAME "
				LOG_MSG "[INFO]:-Master port                   = $MASTER_PORT"
				LOG_MSG "[INFO]:-Master current role           = $QD_ROLE"
				LOG_MSG "[INFO]:-MPP software version          = $MPP_VERSION"
				LOG_MSG "[INFO]:-Postgres version              = $QD_PG_VERSION"
				LOG_MSG "[INFO]:-MPP failure action            = $FAIL_TYPE"
				LOG_MSG "[INFO]:-MPP mirroring status          = $MIRROR_CONFIG"
				LOG_MSG "[INFO]:-MPP master standby            = $STANDBY_CONFIGURED"
				LOG_MSG "[INFO}:-MPP master standby state      = $STANDBY_STATE"
				LOG_MSG "[INFO]:-Segment instance status"
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
						STATUS=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};export PGPORT=${QE_PORT};$PG_CTL status -D $QE_DIR"|$HEAD -1|$GREP -c PID` 2>> $LOG_FILE
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
		LOG_MSG "[INFO]:-Segment Instance Status Report"
		LOG_MSG "[INFO]:-------------------------------"
		LOG_MSG "[INFO]:-Segment instance's reported active via pg_ctl = $QD_STATE_COUNT "
		if [ $QD_FAILED_COUNT -ne 0 ];then
		LOG_MSG "[WARN]:-Segment instance's reported failed via pg_ctl = $QD_FAILED_COUNT <<<<<<<<<<"
		else
		LOG_MSG "[INFO]:-Segment instance's reported failed via pg_ctl = $QD_FAILED_COUNT"
		fi
		LOG_MSG "[INFO]:-Segment instance's marked as vaild            = $QD_VALID_COUNT"
		if [ $QD_NOTVALID_COUNT -ne 0 ];then
		LOG_MSG "[WARN]:-Segment instance's marked as invalid          = $QD_NOTVALID_COUNT <<<<<<<<<<"
		else
		LOG_MSG "[INFO]:-Segment instance's marked as invalid          = $QD_NOTVALID_COUNT"
		fi
fi
}

VERSION_INFO () {
	GET_QE_DETAILS
	MASTER_POSTGRES_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $DEFAULTDB -c"show server_version;"`
	ERROR_CHK $? "obtain postgres server version" 1
	MASTER_INITDB_VERSION=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $DEFAULTDB -c"select productversion from $MPP_INITDB_VER_TABLE limit 1;"|$TR '$' ' '|$TR -s ' '`
	LOG_MSG "[INFO]:-Master segment version details"
	LOG_MSG "[INFO]:-------------------------------"
	LOG_MSG "[INFO]:-Master segment      = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-Postgres version    = $MASTER_POSTGRES_VERSION"
	LOG_MSG "[INFO]:-MPP version         = $MASTER_INITDB_VERSION"
	LOG_MSG "[INFO]:-Segment instance version details"
	LOG_MSG "[INFO]:---------------------------------"
	for TNODE in "${QE_ARRAY[@]}"
	do
		SET_VAR
		POSTGRES_VERSION=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -A -t -p $QE_PORT $DEFAULTDB -c\"show server_version;\""`
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
			LOG_MSG "[WARN]:-Unable to connect to $QE_NAME on port $QE_PORT"
			ERROR_EXIT=1
		else
			LOG_MSG "[INFO]:-$QE_NAME segment    = $QE_DIR"
			LOG_MSG "[INFO]:-Postgres version    = $POSTGRES_VERSION"
		fi
		INITDB_VERSION=`$TRUSTED_SHELL $QE_NAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -A -t -p $QE_PORT $DEFAULTDB -c\"select productversion from $MPP_INITDB_VER_TABLE;\""|$TR '$' ' '|$TR -s ' '`
		RETVAL=$?
		LOG_MSG "[INFO]:-MPP version         = $INITDB_VERSION"
	done

}

MIRROR_LIST () {
		GET_QE_MIRROR_DETAILS
		EXIT_STATUS=0
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
							LM=WARN
							TAG="<<<<<<<<<<"
							EXIT_STATUS=1
						else
							if [ "$QE_PRIMARY" == "f" ] && [ "$QE_VALID" == 't' ]; then
								STATUS="Passive"
								LM=INFO
								TAG=""
							else
								STATUS="Failed"
								LM=WARN
								TAG="<<<<<<<<<<"
								EXIT_STATUS=1
							fi
						fi
						$TABECHO "[$LM]:-Mirror  = $QE_NAME data directory = $QE_DIR port = $QE_PORT status = $STATUS $TAG"
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
				ERROR_EXIT "[WARN]:-Master instance down" 1
		else
				GET_QE_PRIMARY_DETAILS
				GET_QE_MIRROR_DETAILS
				GET_TRANS_READ
		fi
		LOG_MSG "[INFO]:-Processing status request, please wait"
		GET_STANDBY_DETAILS
		if [ ${#STANDBY_ARRAY[@]} -eq 0 ];then
			STANDBY_TEXT="NOT CONFIGURED"
		else
			MASTER_STANDBY_HOSTNAME=`$ECHO $STANDBY_ARRAY|$CUT -d"|" -f1`
			MASTER_STANDBY_PORT=`$ECHO $STANDBY_ARRAY|$CUT -d"|" -f3`
			GET_PG_PID_ACTIVE $MASTER_STANDBY_PORT $MASTER_STANDBY_HOSTNAME
			if [ $PID -eq 0 ];then
				STANDBY_TEXT="Down <<<<<<<<<<"
			else
				STANDBY_TEXT="Passive"
			fi
		fi
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
		LOG_MSG "[INFO]:-Status request sent to $QE_NAME $QE_DIR"
		if [ $RETVAL -ne 0 ]; then
				((QE_PRIMARY_FAILED_COUNT=$QE_PRIMARY_FAILED_COUNT+1))
		else
				#Have a pid, but is it still running?
				GET_PG_PID_ACTIVE $QE_PORT $QE_NAME
				QE_PID_COUNT=$PID
				#QE_PID_COUNT=`$TRUSTED_SHELL $QE_NAME "$PS $PS_TXT|$AWK '{print \\$1}'|$GREP -c \"^${QE_PID}\$\""`
				if [ $QE_PID_COUNT -eq 0 ];then
				#We have a postmaster.pid but no process running
					((QE_PRIMARY_FAILED_COUNT=$QE_PRIMARY_FAILED_COUNT+1))
				else
					((QE_PRIMARY_STATE_COUNT=$QE_PRIMARY_STATE_COUNT+1))
				fi
		fi
		((QE_COUNT=$QE_COUNT+1))
done
for TNODE in "${QE_MIRROR_ARRAY[@]}"
		do
		SET_VAR
		QE_VALID=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME  -A -t -c"select valid from $MPP_PG_VIEW where dbid in (select dbid from $CONFIG_TABLE where port=$QE_PORT and datadir='${QE_DIR}');"`
		QE_PRIMARY=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME  -A -t -c"select isprimary from $MPP_PG_VIEW where dbid in (select dbid from $CONFIG_TABLE where port=$QE_PORT and datadir='${QE_DIR}');"`
		
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
		LOG_MSG "[INFO]:-Status request sent to $QE_NAME $QE_DIR"
if [ $RETVAL -ne 0 ]; then
		((QE_MIRROR_FAILED_COUNT=$QE_MIRROR_FAILED_COUNT+1))
else
		#QE_PID_COUNT=`$TRUSTED_SHELL $QE_NAME "$PS $PS_TXT|$AWK '{print \\$1}'|$GREP -c \"^${QE_PID}\$\""`
		GET_PG_PID_ACTIVE $QE_PORT $QE_NAME
		QE_PID_COUNT=$PID
		if [ $QE_PID_COUNT -eq 0 ];then
			((QE_MIRROR_FAILED_COUNT=$QE_MIRROR_FAILED_COUNT+1))
		else
			((QE_MIRROR_STATE_COUNT=$QE_MIRROR_STATE_COUNT+1))
		fi
fi
((QE_COUNT=$QE_COUNT+1))
		done
		LOG_MSG "[INFO]:-MPP instance status summary"
		LOG_MSG "[INFO]:-------------------------------------------------------"
		LOG_MSG "[INFO]:-Master instance                               = Active" 
		LOG_MSG "[INFO]:-Master instance read-only                     = $TRANS_STATE"
		LOG_MSG "[INFO]:-Master instance standby                       = $STANDBY_TEXT"
		LOG_MSG "[INFO]:---------------------------"
		LOG_MSG "[INFO]:-Total segment instance count from meta data   = $QE_COUNT"
		LOG_MSG "[INFO]:-Total segment instance primary PID active     = $QE_PRIMARY_STATE_COUNT"
		if [ $QE_PRIMARY_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total segment instance primary PID failed     = $QE_PRIMARY_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total segment instance primary PID failed     = $QE_PRIMARY_FAILED_COUNT <<<<<<<<<<"
				EXIT_STATUS=1
		fi	
		LOG_MSG "[INFO]:-Total segment instance primary db active      = $QE_PRIMARY_VALID"
		if [ $QE_PRIMARY_INVALID -eq 0 ]; then
			LOG_MSG "[INFO]:-Total segment instance primary db failed      = $QE_PRIMARY_INVALID"
		else
			LOG_MSG "[WARN]:-Total segment instance primary db failed      = $QE_PRIMARY_INVALID <<<<<<<<<<"
			EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-Total segment instance mirror PID active      = $QE_MIRROR_STATE_COUNT"
		if [ $QE_MIRROR_FAILED_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Total segment instance mirror PID failed      = $QE_MIRROR_FAILED_COUNT"
		else
				LOG_MSG "[WARN]:-Total segment instance mirror PID failed      = $QE_MIRROR_FAILED_COUNT <<<<<<<<<<"
				EXIT_STATUS=1
		fi
		if [ $QE_MIRROR_ACTIVE -ne 0 ];then
			LOG_MSG "[WARN]:-Total segment instance mirror active primary  = $QE_MIRROR_ACTIVE <<<<<<<<<<"
			EXIT_STATUS=1
		else
			LOG_MSG "[INFO]:-Total segment instance mirror active primary  = $QE_MIRROR_ACTIVE"
		fi
		if [ $QE_MIRROR_FAILED -ne 0 ];then
			LOG_MSG "[WARN]:-Total segment instance mirror failed          = $QE_MIRROR_FAILED <<<<<<<<<<"
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
		PORT_ARRAY=(`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname, port, datadir from $CONFIG_TABLE where content<>-1;" `)
		ERROR_CHK $? "obtain port assignments" 2
		LOG_MSG "[INFO]:-Master segment instance  $MASTER_DATA_DIRECTORY  port = $MASTER_PORT"
		LOG_MSG "[INFO]:-Segment instance port assignments"
		LOG_MSG "[INFO]:----------------------------------"
		for PMODE in "${PORT_ARRAY[@]}"
				do
				HOST=`$ECHO $PMODE|$CUT -d"|" -f1`
				PORT=`$ECHO $PMODE|$CUT -d"|" -f2`
				DIR=`$ECHO $PMODE|$CUT -d"|" -f3`
				LOG_MSG "[INFO]:-Segment instance $HOST $DIR  port = $PORT"
		done
}

LIST_MPP_LOGS () {
		LOG_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select hostname, datadir from $CONFIG_TABLE where content<>-1 ;" 2>|/dev/null`)
		ERROR_CHK $? "obtain log assignments" 2
		LOG_MSG "[INFO]:-Master segment instance $MASTER_DATA_DIRECTORY logfile ${MASTER_DATA_DIRECTORY}.log"
		LOG_MSG "[INFO]:-Segment instance database logfiles"
		LOG_MSG "[INFO]:-----------------------------------"
		for LOGCHK in "${LOG_ARRAY[@]}"
				do
				HOST=`$ECHO $LOGCHK|$CUT -d"|" -f1`
				DIR=`$ECHO $LOGCHK|$CUT -d"|" -f2`
				if [ $TAIL_LOGS ]; then
					LOG_MSG "[INFO]:-$HOST $DIR log file tail START---------------"
					$ECHO "$HOST Logfile name ${DIR}.log"
					if [ x'' == x'$GREP_STRING' ];then
						$TRUSTED_SHELL $HOST "$TAIL -$TLINES ${DIR}.log"|$TEE -a $LOG_FILE
					else
						$TRUSTED_SHELL $HOST "$TAIL -$TLINES ${DIR}.log|$EGREP '$GREP_STRING'"|$TEE -a $LOG_FILE
					fi
						$ECHO
						LOG_MSG "[INFO]:-$HOST $DIR log file tail END-----------------"
				else
						LOG_MSG "[INFO]:-$HOST $DIR logfile ${DIR}.log"
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
			STANDBY_STATE="STANDBY HOST ACTIVE INSTANCE MASTER"
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

DISPLAY_RO_MASTER () {
	LOG_MSG "[INFO]:----------------------------------------"
	LOG_MSG "[INFO]:-Master read only hostname = `$HOSTNAME`"
	LOG_MSG "[INFO]:-Master read only port     = $MASTER_PORT"
	LOG_MSG "[INFO]:-Master read only status   = Active"
	LOG_MSG "[INFO]:----------------------------------------"
}
#******************************************************************************
# Main Section
#******************************************************************************
#Check to see if we have a parameter supplied to script
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
SELECT=1
if [ $# -ne 0 ]; then
		while getopts ":vtbl:d:nieqsmcpf'?'r:g:" opt;do
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
						r ) TLINES=$OPTARG ;;
						l ) LOG_DIR=$OPTARG ;;
						g ) GREP_STRING=$OPTARG ;;
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
READ_CHK
if [ $READ_COUNT -ne 0 ];then
	DISPLAY_RO_MASTER
	LOG_MSG "[INFO]:-End Main"
	exit 0
fi
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
