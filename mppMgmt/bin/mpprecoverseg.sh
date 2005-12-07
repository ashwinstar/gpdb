#!/bin/bash
#	Filename:-		mpprecoverseg.bsh
#	Version:-		$Revision$
#	Updated:-		$Date$
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
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Recovers mpp segment instances that are marked as invalid, if"
		$ECHO "      mirroring is configured and operational"
		$ECHO
        	$ECHO "      Usage: "
		$ECHO "      $0 [OPTIONS]"
		$ECHO
		$ECHO "      General options:"
		$ECHO "      -?,display this help message & exit"
		$ECHO "      -v, display version information & exit"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "      -a, don't ask to confirm recovery [default:- ask]"
		$ECHO 
		$ECHO "      Recovery options:"
		$ECHO "      -f, force MPP cluster shutdown and restart"
        	$ECHO "      -z, <failed_segment_instance_directory_name:failed_hostname> force a recovery,"
		$ECHO "          even though no instance segments are marked as invalid on the Master"
		$ECHO "          CAUTION:-USE THIS OPTION WITH CARE!"
        	$ECHO "          example $0 -z /dbfast1/segment1:localhost"
        	$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master database directory> name of alternative Master database to start up [optional]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but recovery completed"
		$ECHO "      2 Fatal error, recovery failed"
		$ECHO
        	exit 0
}

GET_SETTING () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	FAIL_TYPE=`$PSQL -p $MASTER_PORT $QD_DBNAME -R" " -A -t -c"show mpp_fault_action;"`
	ERROR_CHK $? "obtain mpp_fault_action setting" 2
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
	CHK_MIRRORS_CONFIGURED
	if [ $MIRROR_COUNT -ne 2 ]; then
		LOG_MSG "[FATAL]:-Mirroring not configured on this MPP cluster"
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	fi
	#Get a list of databases contained within the array
	DB_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datname from pg_database where datallowconn<>'f';"`)
	ERROR_CHK $? "obtain list of database names from master instance" 1
	#Get a list of dbid's that are marked as invalid
	for DBNAME in ${DB_LIST[@]}
	do
		FAILED_INST_LIST=(`$PSQL -p $MASTER_PORT $DBNAME -R" " -A -t -c"select dbid from mpp.mpp_pgdatabase where valid='f';"`)
		ERROR_CHK $? "obtain master instance failed segments" 2
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
	TOTAL_INST_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid) from mpp_configuration where content<>-1;"`
	ERROR_CHK $? "obtain total segement count from master instance" 2
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
	FAILED_INST_LIST=(`$PSQL -p $MASTER_PORT $DBNAME -R" " -A -t -c"select dbid from mpp_configuration where valid='f';"`)
	ERROR_CHK $? "obtain failed DBID details from master instance" 2
	for DBID in ${FAILED_INST_LIST[@]}
	do
		FAILED_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DBNAME -A -t -c"select hostname, port, datadir, dbid from mpp_configuration where valid='f' and dbid=${DBID};"`
		ERROR_CHK $? "obtain failed segment details from master instance" 2
		ACTIVE_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DBNAME -A -t -c"select hostname, port, datadir, dbid from mpp_configuration where valid='t' and content in (select content from mpp_configuration where dbid=${DBID});"`
		ERROR_CHK $? "obtain recovery segment details from master instance" 2
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
		LOG_MSG "[INFO]:-Procedure will STOP database as mpp_fault_action=$FAIL_TYPE"
	else
		LOG_MSG "[INFO]:-Procedure can continue without database interruption as mpp_fault_action=$FAIL_TYPE"
	fi
	$ECHO
        $ECHO "Continue with recovery procedure yY|nN >"
        read REPLY
	if [ x"" == x"$REPLY" ];then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!"
		exit 1;fi
        if [ $REPLY != Y ] && [ $REPLY != y ];then
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
		RUN_COMMAND_REMOTE $FAIL_QE_HOST "$ECHO test >/tmp/remote_test1.$$" 
		LOG_MSG "[INFO]:-Test 2, create temporary file on ${MIR_QE_HOST}"
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$ECHO test >/tmp/remote_test2.$$" 
		LOG_MSG "[INFO]:-Test 3, copy file from ${MIR_QE_HOST} to ${FAIL_QE_HOST}"
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$SCP -pr /tmp/remote_test2.$$ ${FAIL_QE_HOST}:/tmp"
		LOG_MSG "[INFO]:-Clearing temporary files"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f /tmp/remote_test1.$$" 
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -f /tmp/remote_test2.$$" 
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$RM -f /tmp/remote_test2.$$" 
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
        ${WORKDIR}/$STOP_PROG -zfa
	if [ x"" == x"$FORCE_RECOVER_STRING" ];then
        	ERROR_CHK $? "shutdown master array" 2
	else
		LOG_MSG "[INFO]:-Ignoring shutdown errors as force option selected"
	fi
        LOG_MSG "[INFO]:-Successfully shutdown the MPP array"
	else
		while [ $COUNT -lt $ARRAY_INDEX ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Forcing shutdown of $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m immediate -l ${FAIL_QE_DIR}.log"
			ERROR_CHK $? "force shutdown of $FAIL_QE_DIR on $FAIL_QE_HOST" 1
			LOG_MSG "[INFO]:-Clearing /tmp/lock files for $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_NAME "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock /tmp/.s.PGSQL.${FAIL_QE_PORT}"
			ERROR_CHK $? "remove of /tmp lockfiles for $FAIL_QE_HOST" 1
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
		GET_PG_PID_ACTIVE $FAIL_QE_PORT $FAIL_QE_HOST
		if [ $PID -ne 0 ]; then
			$TRUSTED_SHELL $FAIL_QE_HOST "$KILL -9 $PID" > $LOG_FILE 2>&1
		else
			LOG_MSG "[INFO]:-No active pid found for $FAIL_QE_HOST for port=$FAIL_QE_PORT"
		fi
		LOG_MSG "[INFO]:-Deleting failed database directory on ${FAIL_QE_HOST}"
		RUN_COMMAND_REMOTE ${FAIL_QE_HOST} "$RM -Rf ${FAIL_QE_DIR}" 
		#Run a checkpoint if STOPDB=0
		if [ $STOPDB -eq 0 ]; then
			LOG_MSG "[INFO]:-Issuing checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT"
			$TRUSTED_SHELL $MIR_QE_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MIR_QE_PORT $DEFAULTDB -c \"checkpoint;\""
			ERROR_CHK $? "instance checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT" 1
		fi
		LOG_MSG "[INFO]:-Recovering data directory from ${MIR_QE_HOST} to ${FAIL_QE_HOST}"
		LOG_MSG "[INFO]:-This could take some time depending on data volume ... Please Wait ..."
		KB_COPY=`$TRUSTED_SHELL ${MIR_QE_HOST} "$DU -k -s $DU_TXT ${MIR_QE_DIR}|$TAIL -1"|$AWK '{print $1}'`
		LOG_MSG "[INFO]:-Copy will transfer $KB_COPY KBytes of data" 
		RUN_COMMAND_REMOTE ${MIR_QE_HOST} "$TRUSTED_COPY -pr ${MIR_QE_DIR} ${FAIL_QE_HOST}:${FAIL_QE_DIR}" 
		LOG_MSG "[INFO]:-Setting recovered port to ${FAIL_QE_PORT} on ${FAIL_QE_HOST}" 
		ED_FILE ${FAIL_QE_DIR}/$PG_CONF "port=" "port=${FAIL_QE_PORT}" 0 ${FAIL_QE_HOST}
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
		$TRUSTED_SHELL $FAIL_QE_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -w -D $FAIL_QE_DIR -l ${FAIL_QE_DIR}.log -o \"-i -p $FAIL_QE_PORT \" start" 
		ERROR_CHK $? "segment start $FAIL_QE_HOST instance $FAIL_QE_DIR port=$FAIL_QE_PORT" 2
		for MPP_DB in ${DB_LIST[@]}
		do
			LOG_MSG "[INFO]:-Updating mpp_mpp on $FAIL_QE_HOST database $MPP_DB"
			$TRUSTED_SHELL $FAIL_QE_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $FAIL_QE_PORT $QD_DBNAME -c \"update mpp_mpp set dbid=${FAIL_QE_DBID} where dbid<>${FAIL_QE_DBID};\""
			ERROR_CHK $? "update segment mpp_mpp on $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT" 2
		done
		if [ $STOPDB -eq 1 ]; then
				LOG_MSG "[INFO]:-Commencing shutdown of $FAIL_QE_HOST port=$FAIL_QE_PORT"
				$TRUSTED_SHELL $FAIL_QE_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m smart -l ${FAIL_QE_DIR}.log"
				ERROR_CHK $? "segment instance stop $FAIL_QE_HOST port=$FAIL_QE_PORT" 2
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
		$PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from ${TABLE_NAME};" >> $LOG_FILE 2>&1
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
				ERROR_CHK $? "update status for database $MPP_DB on Master database" 2
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
                ERROR_EXIT "[FATAL]:-Issue with master database startup." 2
	fi
        LOG_MSG "[INFO]:-Successfully started the master array"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


FORCE_RECOVER () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	FAIL_QE_DIR=`$ECHO $FORCE_RECOVER_STRING|$CUT -d: -f1`
	FAIL_QE_HOST=`$ECHO $FORCE_RECOVER_STRING|$CUT -d: -f2`
	if [ x"" == x"$FAIL_QE_DIR" ] || [ x"" == x"$FAIL_QE_HOST" ] || [ "$FAIL_QE_DIR" == "$FAIL_QE_HOST" ];then
		ERROR_EXIT "[FATAL]:Option -z string $FORCE_RECOVER_STRING incorrect" 2;fi
	GET_QD_DB_NAME
	#Obtain hostname for segment instance directory that is to be recovered
	FAILED_INSTANCE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from mpp_configuration where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`
	ERROR_CHK $? "obtain failed instance count" 2
	if [ $FAILED_INSTANCE_COUNT -eq 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	if [ $FAILED_INSTANCE_COUNT -gt 1 ];then
		ERROR_EXIT "[FATAL]:-Found multiple records for $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	FAILED_INSTANCE_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname, port, datadir, dbid from mpp_configuration where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`)
	ERROR_CHK $? "obtain failed segment instance details" 2
	FAIL_SET_VAR "${FAILED_INSTANCE_ARRAY[@]}" 
	#Get active mirror details
	ACTIVE_INSTANCE_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname, port, datadir, dbid from mpp_configuration where valid='t' and content in (select content from mpp_configuration where dbid=${FAIL_QE_DBID});"`)
	ERROR_CHK $? "obtain recovery segment instance details" 2
	MIR_SET_VAR "${ACTIVE_INSTANCE_ARRAY[@]}"	
	ARRAY_INDEX=1
	STOPDB=1
	if [ $INTERACTIVE ]; then
		DISPLAY_PARAM
	fi
	TEST_ACCESS
	STOP_DATABASE
	RECOVER_DATA
	UPDATE_MPP_MPP
	START_DATABASE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'qfal:d:z:" opt;do
    case $opt in
        v ) VERSION_INFO ;;
      '?' ) USAGE ;;
	a ) unset INTERACTIVE ;;
        d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
        q ) unset VERBOSE ;;
	f ) FORCESTOP=1 ;;
    	l ) LOG_DIR=$OPTARG ;;
	z ) FORCE_RECOVER_STRING=$OPTARG 
	    FORCE_RECOVER 
	    exit $EXIT_STATUS ;;
        * ) USAGE ;;
    esac
done
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
