#!/bin/bash
#	Filename:-		mpprecoverseg.bsh
#	Version:-		1.0
#	Updated:-		21/09/2005
#	Status:-		Currently undergoing initial testing
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com	
#	Release date:-		TBC
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Script to recover database segments in
#				the event of a node failure
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
#******************************************************************************
# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
WORKDIR=`dirname $0`
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
if [ -f $FUNCTIONS ];then
        . $FUNCTIONS
else
        echo "[FATAL:]-Cannot source $FUNCTIONS file Script Exits!"
        exit 1;fi
#******************************************************************************
# Location Variables
#******************************************************************************
declare -a FAILED_HOST_ARRAY
declare -a FAILED_DATADIR_ARRAY
declare -a FAILED_DATADIR_ARRAY_SUB
declare -a FAILED_PORT_ARRAY
declare -a MIRROR_HOST_ARRAY
declare -a MIRROR_DATADIR_ARRAY
STOP_PROG=mppstop.sh
START_PROG=mppstart.sh
#******************************************************************************
# Command Variables
#******************************************************************************

#******************************************************************************
# Script Specific Variables
#******************************************************************************
INTERACTIVE=1
VERBOSE=1
WORKDIR=`pwd`
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
PG_PID=postmaster.pid
EXIT_STATUS=0
FORCESTOP=1
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
        $ECHO
		$ECHO $0
		$ECHO "Recovers mpp segment instances that are marked as invalid, if"
		$ECHO "mirroring is configured and operational"
		$ECHO
        $ECHO "Usage: "
		$ECHO $0 "[OPTIONS]"
		$ECHO
		$ECHO "General options:"
		$ECHO "-?,display this help message & exit"
		$ECHO "-v, display version information & exit"
		$ECHO
		$ECHO "Logging options:"
		$ECHO "-q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "-l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "-a, don't ask to confirm recovery [default:- ask]"
		$ECHO 
		$ECHO "Recovery options:"
		$ECHO "-f, force MPP cluster shutdown and restart"
        $ECHO
		$ECHO "Connection options:"
		$ECHO "-d, <QD database directory> name of alternative QD database to start up [optional]"
		$ECHO
		$ECHO "Return codes:"
		$ECHO "0 No problems encountered with requested operation"
		$ECHO "1 Warning generated, but recovery completed"
		$ECHO "2 Fatal error, recovery failed"
        exit 1
}

GET_SETTING () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	FAIL_TYPE=`$PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"show MPPFaultAction;"`
	if [ "$FAIL_TYPE" == "readonly" ];then
		STOPDB=0
	else
		STOPDB=1
	fi
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
}

GET_MIRROR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Now check to see that we have mirroring configured on this system
	MIRROR_COUNT=`$PSQL -p $MASTER_PORT template1 -A -t -c"select count(dbid)/count(distinct(content)) from mpp.pgdatabase where content<>-1;"`
	if [ $MIRROR_COUNT -ne 2 ]; then
		LOG_MSG "[FATAL]:-Mirroring not configured on this MPP cluster"
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	fi
	#Get a list of databases contained within the array
	DB_LIST=(`$PSQL -p $MASTER_PORT template1 -R" " -A -t -c"select datname from pg_database where datallowconn<>'f';"`)
	#Get a list of dbid's that are marked as invalid
	for DBNAME in ${DB_LIST[@]}
	do
##		FAILED_INST_LIST=(`$PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"select dbid from mpp.mpp_pgdatabase where valid='f';"`)
		FAILED_INST_LIST=(`$PSQL -p $MASTER_PORT $DBNAME -R" " -A -t -c"select dbid from mpp.mpp_pgdatabase where valid='f';"`)
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
			ERROR_EXIT "[FATAL]:-Unable to access master database"
		fi
		FAILED_INST_COUNT=${#FAILED_INST_LIST[@]}
		if [ $FAILED_INST_COUNT -eq 0 ]; then
			LOG_MSG "[INFO]:-No invalid segment instances located for database $DBNAME"
		else
			LOG_MSG "[INFO]:-Located $FAILED_INST_COUNT invalid segment instances for database $DBNAME"
			DB_MARKED_INVALID=($DB_MARKED_INVALID $DBNAME)
		fi
	done
	if [ ${#DB_MARKED_INVALID[@]} -eq 0 ];then
		LOG_MSG "[INFO]:-No invalid segment instances located on array, script exits"
		exit 0
	fi

	#Now check to make sure that we can initiate recovery
	TOTAL_INST_COUNT=`$PSQL -p $MASTER_PORT template1 -A -t -c"select count(dbid) from mpp.pgdatabase where content<>-1;"`
	if [ $FAILED_INST_COUNT -gt $TOTAL_INST_COUNT ]; then
		LOG_MSG "[FATAL]:-Insufficient remaining active instances to be able to recover cluster"
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	else
		LOG_MSG "[INFO]:-Sufficient active instances to be able to start recovery"
	fi

	#Now obtain details of failed instances and also their associated mirrors
	LOG_MSG "[INFO]:-Obtaining recovery information"
	ARRAY_INDEX=0
	DBNAME=${DB_MARKED_INVALID[0]}
	FAILED_INST_LIST=(`$PSQL -p $MASTER_PORT $DBNAME -R" " -A -t -c"select dbid from mpp.mpp_pgdatabase where valid='f';"`)
	for DBID in ${FAILED_INST_LIST[@]}
	do
		FAILED_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DBNAME -A -t -c"select a.hostname, b.port, b.datadir, c.dbid from mpp.host a, mpp.listener b, mpp.mpp_pgdatabase c where a.hostid=b.hostid and b.listenerid=c.listenerid and c.valid='f' and c.dbid=${DBID};"`
		ACTIVE_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DBNAME -A -t -c"select a.hostname, b.port, b.datadir, c.dbid from mpp.host a, mpp.listener b, mpp.mpp_pgdatabase c where a.hostid=b.hostid and b.listenerid=c.listenerid and c.valid='t' and c.content in (select content from mpp.pgdatabase where dbid=${DBID});"`
		((ARRAY_INDEX=$ARRAY_INDEX+1))
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Node recovery parameters"
	LOG_MSG "[INFO]:-------------------------"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		((REC_COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Recovery $REC_COUNT of $ARRAY_INDEX"
		LOG_MSG "[INFO]:-------------------------------"
		LOG_MSG "[INFO]:-Failed instance details    = ${FAILED_INSTANCE_ARRAY[$COUNT]}"
		LOG_MSG "[INFO]:-Recovery instance details  = ${ACTIVE_INSTANCE_ARRAY[$COUNT]}"
		LOG_MSG "[INFO]:-------------------------------"
		((COUNT=$COUNT+1))
	done
	$ECHO
	if [ $STOPDB -eq 1 ]; then
		LOG_MSG "[INFO]:-Procedure will STOP database as MPPFaultAction=$FAIL_TYPE"
	else
		LOG_MSG "[INFO]:-Procedure can continue without database interruption as MPPFaultAction=$FAIL_TYPE"
	fi
	$ECHO
        $ECHO -n "Continue with recovery procedure yY|nN >"
        read REPLY
        if [ $REPLY != Y ] && [ $REPLY != y ] || [ -z $REPLY ];then
                LOG_MSG "[WARN]:-User abort requested, Script Exits!"
                exit 1;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

FAIL_SET_VAR () {
	FAIL_QE_HOST=`$ECHO $1|$CUT -d"|" -f1`
	FAIL_QE_PORT=`$ECHO $1|$CUT -d"|" -f2`
	FAIL_QE_DIR=`$ECHO $1|$CUT -d"|" -f3`
	FAIL_QE_DBID=`$ECHO $1|$CUT -d"|" -f4`
	FAIL_QE_TOP_DIR=`$DIRNAME $FAIL_QE_DIR`
}

MIR_SET_VAR () {
	MIR_QE_HOST=`$ECHO $1|$CUT -d"|" -f1`
	MIR_QE_PORT=`$ECHO $1|$CUT -d"|" -f2`
	MIR_QE_DIR=`$ECHO $1|$CUT -d"|" -f3`
	MIR_QE_DBID=`$ECHO $1|$CUT -d"|" -f4`
	MIR_QE_TOP_DIR=`$DIRNAME $MIR_QE_DIR`

}
TEST_ACCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Testing remote access"
	LOG_MSG "[INFO]:-Trusted shell = $TRUSTED_SHELL"
	LOG_MSG "[IMFO]:-Trusted copy  = $TRUSTED_COPY"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		MIR_SET_VAR ${ACTIVE_INSTANCE_ARRAY[$COUNT]}
		((REC_COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Test Cycle $REC_COUNT of $ARRAY_INDEX"
		LOG_MSG "[INFO]:----------------------------------"
		LOG_MSG "[INFO]:-Test 1, create temporary file on $FAIL_QE_HOST"
		RUN_COMMAND_REMOTE $FAIL_QE_HOST "$ECHO test >/tmp/remote_test1" 
		LOG_MSG "[INFO]:-Test 2, create temporary file on ${MIR_QE_HOST}"
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$ECHO test >/tmp/remote_test2" 
		LOG_MSG "[INFO]:-Test 3, copy file from ${MIR_QE_HOST} to ${FAIL_QE_HOST}"
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$SCP -pr /tmp/remote_test2 ${FAIL_QE_HOST}:/tmp"
		LOG_MSG "[INFO]:-Clearing temporary files"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f /tmp/remote_test1" 
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f /tmp/remote_test2" 
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$RM -f /tmp/remote_test2" 
		((COUNT=$COUNT+1))	
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
STOP_DATABASE () { 
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $STOPDB -eq 1 ] && [ $FORCESTOP -ne 0 ]; then
		CHK_FILE ${WORKDIR}/$STOP_PROG
        if [ $EXISTS -ne 0 ];then
                ERROR_EXIT "[FATAL]:-Could not locate $WORKDIR/$STOP_PROG script"
		fi
        LOG_MSG "[INFO]:-Shutting down the Cluster ... Please wait ..."
        ${WORKDIR}/$STOP_PROG -za
        RETVAL=$?
        if [ $RETVAL -gt 1 ];then
                ERROR_EXIT "[FATAL]:-Could not shutdown the MPP array"
		fi
        LOG_MSG "[INFO]:-Successfully shutdown the MPP array"
	else
		while [ $COUNT -lt $ARRAY_INDEX ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Forcing shutdown of $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m immediate -l ${FAIL_QE_DIR}.log"
			LOG_MSG "[INFO]:-Clearing /tmp/lock files for $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_NAME "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock /tmp/.s.PGSQL.${FAIL_QE_PORT}"
			((COUNT=$COUNT+1))		
		done
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RECOVER_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		MIR_SET_VAR ${ACTIVE_INSTANCE_ARRAY[$COUNT]}
		LOG_MSG "[INFO]:-Checking for invalid postmaster process on port ${FAIL_QE_HOST}"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$PS ax|$GREP postmaster|$GREP '\-p ${FAIL_QE_PORT}'|$GREP -v grep|$AWK '{print \$1}' >${FAIL_QE_TOP_DIR}/kill.pid" 
		$TRUSTED_SHELL ${FAIL_QE_HOST} "kill -9 \`cat ${FAIL_QE_TOP_DIR}/kill.pid\`" 2>> $LOG_FILE
		LOG_MSG "[INFO]:-Deleting failed database directory on ${FAIL_QE_HOST}"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -Rf ${FAIL_QE_DIR}" 
		#Run a checkpoint if STOPDB=0
		if [ $STOPDB -eq 0 ]; then
			LOG_MSG "[INFO]:-Issuing checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT"
			$TRUSTED_SHELL $FAIL_QE_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MIR_QE_PORT template1 -c \"checkpoint;\""
			RETVAL=$?
			if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Checkpoint failed on $MIR_QE_HOST port=$MIR_QE_PORT"
				EXIT_STATUS=1
			else
				LOG_MSG "[INFO]:-Completed checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT"
			fi
		fi
		LOG_MSG "[INFO]:-Recovering data directory from ${MIR_QE_HOST} to ${FAIL_QE_HOST}"
		LOG_MSG "[INFO]:-This could take some time depending on data volume ... Please Wait ..."
		KB_COPY=`$TRUSTED_SHELL ${MIR_QE_HOST} "$DU ${MIR_QE_DIR} -k -s -c|$TAIL -1"|$AWK '{print $1}'`
		((COPY_TIME=$KB_COPY/28672))
		LOG_MSG "[INFO]:-Estimated duration of copy $COPY_TIME seconds"
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$TRUSTED_COPY -pr ${MIR_QE_DIR} ${FAIL_QE_HOST}:${FAIL_QE_DIR}" 
		LOG_MSG "[INFO]:-Setting recovered port to ${FAIL_QE_PORT} on ${FAIL_QE_HOST}" 
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$GREP -v '^port=' ${FAIL_QE_DIR}/postgresql.conf > ${FAIL_QE_DIR}/postgresql.conf_copy"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$ECHO 'port=${FAIL_QE_PORT}' >>${FAIL_QE_DIR}/postgresql.conf_copy"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$MV ${FAIL_QE_DIR}/postgresql.conf_copy ${FAIL_QE_DIR}/postgresql.conf"
		LOG_MSG "[INFO]:-Clearing old postmaster.opts file"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f ${FAIL_QE_DIR}/postmaster.opts"
		if [ $STOPDB -eq 0 ]; then
			LOG_MSG "[INFO]:-Clearing old postmaster.pid file"
			RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f ${FAIL_QE_DIR}/postmaster.pid"
		fi
		((COUNT=$COUNT+1))
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_MPP_MPP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		((COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Updating instance catalog on $FAIL_QE_HOST port=$FAIL_QE_PORT"
		LOG_MSG "[INFO]:-Starting instance on $FAIL_QE_HOST port=$FAIL_QE_PORT"
		$TRUSTED_SHELL $FAIL_QE_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -w -D $FAIL_QE_DIR -l ${FAIL_QE_DIR}.log -o \"-i -p $FAIL_QE_PORT \" start" 
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
			ERROR_EXIT "[FATAL]:-Failed to start $FAIL_QE_HOST instance $FAIL_QE_DIR port=$FAIL_QE_PORT " 2
		else
			LOG_MSG "[INFO]:-Started $FAIL_QE_HOST instance $FAIL_QE_DIR port=$FAIL_QE_PORT"
		fi
		for MPP_DB in ${DB_LIST[@]}
		do
			LOG_MSG "[INFO]:-Updating mpp.mpp on $FAIL_QE_HOST database $MPP_DB"
			$TRUSTED_SHELL $FAIL_QE_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $FAIL_QE_PORT $QD_DBNAME -c \"update mpp.mpp set dbid=${FAIL_QE_DBID} where dbid<>${FAIL_QE_DBID};\""
			RETVAL=$?
			if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Failed to update mpp.mpp on $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT" 2
			else
				LOG_MSG "[INFO]:-Successfully updated mpp.mpp on $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT database $MPP_DB"
			fi
		done
		if [ $STOPDB -eq 1 ]; then
				LOG_MSG "[INFO]:-Commencing shutdown of $FAIL_QE_HOST port=$FAIL_QE_PORT"
				$TRUSTED_SHELL $FAIL_QE_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m smart -l ${FAIL_QE_DIR}.log"
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
					ERROR_EXIT "[FATAL]:-Failed to stop $FAIL_QE_HOST port=$FAIL_QE_PORT" 2
				else
					LOG_MSG "[INFO]:-Successfully stopped $FAIL_QE_HOST port=$FAIL_QE_PORT"
				fi
		fi
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RESET_QE_CONNECT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Obtain a user table that we can issue a select count(*) from QD to force a QE connection re-establishment
	for MPP_DB in ${DB_LIST[@]}
	do
		TABLE_NAME=`$PSQL -p $MASTER_PORT $MPP_DB -A -t -c"select relname from pg_class where relkind='r' order by reltype desc limit(1);"`
		$PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from ${TABLE_NAME};" 2>&1 >> $LOG_FILE
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_STATUS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COUNT=0
		LOG_MSG "[INFO]:-Starting master database in utility mode"
		$WORKDIR/$START_PROG -m
		while [ $COUNT -lt $ARRAY_INDEX ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Setting $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT as active"
			for MPP_DB in ${DB_LIST[@]}
			do
				env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MPP_DB -A -t -c "select mpp.mpp_set_segdb_state (${FAIL_QE_DBID},true);" > /dev/null 2>&1
				if [ $RETVAL -ne 0 ]; then
					ERROR_EXIT "[FATAL]:-Failed to connect to master instance" 2
				else
					LOG_MSG "[INFO]:-Updated status for database $MPP_DB on Master database"
				fi
			done
			((COUNT=$COUNT+1))
		done
		LOG_MSG "[INFO]:-Shutting down master database in utility mode"
		$WORKDIR/$STOP_PROG -m
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_DATABASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
        LOG_MSG "[INFO]:-Starting up the MPP array ... Please wait ..."
        ${WORKDIR}/$START_PROG -a
        RETVAL=$?
        if [ $RETVAL -ne 0 ];then
				LOG_MSG "[WARN]:-The $START_PROG script returned a non zero return code"
				LOG_MSG "[INFO]:-Check the $START_PROG log file for more information on cause"
				LOG_MSG "[INFO]:-Run mppstate.sh to check current status of cluster database"
                ERROR_EXIT "[FATAL]:-Issue with master database startup."
		fi
        LOG_MSG "[INFO]:-Successfully started the master array"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
while getopts ":v'?'qfal:d:" opt;do
    case $opt in
        v ) VERSION_INFO ;;
      '?' ) USAGE ;;
		a ) unset INTERACTIVE ;;
        d ) HEAD_DIRECTORY=$OPTARG ;;
        q ) unset VERBOSE ;;
		f ) FORCESTOP=1 ;;
    	l ) LOG_DIR=$OPTARG ;;
        * ) USAGE ;;
    esac
done
LOG_MSG "[INFO]:-Start Main"
if [ ! $MASTER_DATA_DIRECTORY ]; then
	ERROR_EXIT "[FATAL]:-MASTER_DATA_DIRECTORY environment variable not set" 2
fi
GET_SETTING
GET_MIRROR
if [ $INTERACTIVE ]; then
	DISPLAY_PARAM
fi
TEST_ACCESS
STOP_DATABASE
RECOVER_DATA
UPDATE_MPP_MPP
if [ $STOPDB -eq 1 ] && [ $FORCESTOP -ne 0 ];then
	UPDATE_STATUS
	START_DATABASE
else
	RESET_QE_CONNECT
fi
LOG_MSG "[INFO]:-Recovery completed successfully"
LOG_MSG "[INFO]:-End MAIN"
exit $EXIT_STATUS
