#!/bin/bash
#	Filename:-		mpprecoverseg.bsh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com	
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Script to recover database segments in
#				the event of a node or segment failure
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
FORCESTOP=0
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
		$ECHO "      -f, force MPP instance shutdown and restart"
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
	FAIL_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"show mpp_fault_action;"` >> $LOG_FILE 2>&1
	RET_STATUS=$?
	ERROR_CHK $RET_STATUS "obtain mpp_fault_action setting" 1
	if [ $RET_STATUS -ne 0 ];then
		#If get an error, master segment instance may be down, try to start in admin mode
		$MPPSTART -maq
		ERROR_CHK $? "start master database in admin mode" 2
		#Get a list of dbid's that are marked as invalid
		CHK_MIRRORS_CONFIGURED
		if [ $MIRROR_COUNT -eq 1 ];then
			LOG_MSG "[FATAL]:-There are no mirrors configured for this MPP instance"
			LOG_MSG "[FATAL]:-The failed segment(s) with dbid's ${FAILED_INST_LIST[@]} will"
			LOG_MSG "[FATAL]:-need to be recovered manually, or a complete database restore"
			LOG_MSG "[FATAL]:-will need to be undertaken"
			$MPPSTOP -maq
			ERROR_CHK $? "shutdown master database from admin mode" 2
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		else
			LOG_MSG "[WARN]:-Unable to establish if any instances marked as invalid"
			LOG_MSG "[WARN]:-as master database started in admin mode"
			LOG_MSG "[INFO]:-Restart the database and re-run this utility to check for failures"
			LOG_MSG "[INFO]:-Commencing shutdown from admin mode"
			$MPPSTOP -maq
			ERROR_CHK $? "shutdown master database from admin mode" 2
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		fi
	else
		LOG_MSG "[INFO]:-Fault tolerance is set to $FAIL_TYPE"
		CHK_MIRRORS_CONFIGURED
		if [ $MIRROR_COUNT -eq 1 ];then
			LOG_MSG "[FATAL]:-There are no mirrors configured for this MPP instance"
			LOG_MSG "[FATAL]:-The MPP instance will need to be recovered manually"
			LOG_MSG "[FATAL]:-See mpp_restore command"
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		fi
		FAILED_INST_LIST=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where valid='f';"`)
		RET_STATUS=$?
		ERROR_CHK $RET_STATUS "obtain failed instance dbid's" 1
		LOG_MSG "[INFO]:-There are ${#FAILED_INST_LIST[@]} failed segment(s)"
		if [ $FAIL_TYPE != "readonly" ] && [ $FAIL_TYPE != "continue" ];then
			LOG_MSG "[FATAL]:-Parameter mpp_fault_action is not set to either readonly or continue"
			LOG_MSG "[FATAL]:-MPP fault tolerance is not set"
			LOG_MSG "[FATAL]:-Unable to determine if there are actual segment failures"
			INFO_TEXT
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		else
			case $FAIL_TYPE in
				readonly ) STOPDB=0 
					   LOG_MSG "[INFO]:-Setting STOPDB to false" ;;
				continue ) STOPDB=1
					   LOG_MSG "[INFO]:-Setting STOPDB to true" ;;
			esac
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

INFO_TEXT () {
	LOG_MSG "[INFO]:-To be able to progress this issue, complete the following steps"
	LOG_MSG "[INFO]:-Step 1 Shutdown the MPP instance with mppstop.sh -zfa"
	LOG_MSG "[INFO]:-Step 2 Edit the Master postgresql.conf file and set mpp_fault_action=readonly"
	LOG_MSG "[INFO]:-Step 3 Restart the MPP instance mppstart.sh -a"
	LOG_MSG "[INFO]:-Step 4 Re-run this utility"
}

GET_MIRROR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	FAILED_INST_COUNT=${#FAILED_INST_LIST[@]}
	if [ $FAILED_INST_COUNT -eq 0 ]; then
		LOG_MSG "[INFO]:-No invalid segment instances located"
		LOG_MSG "[INFO]:-Commencing general user access check"
		$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"show mpp_fault_action;" >> $LOG_FILE 2>&1
		RET_STATUS=$?
		if [ $RET_STATUS -ne 0 ];then
			LOG_MSG "[WARN]:-Issue with general access to the database"
			if [ $FAIL_TYPE != "readonly" ];then
				LOG_MSG "[INFO]:-Fault tolerance is set to $FAIL_TYPE"
				LOG_MSG "[INFO]:-Please review the following"
				INFO_TEXT
				ERROR_EXIT "[FATAL]:-Unable to continue" 2
			fi
		else
			LOG_MSG "[INFO]:-General access checked passed"
		fi
		exit 0
	else
		LOG_MSG "[INFO]:-Located $FAILED_INST_COUNT invalid segment instances"
	fi
	#Now check to make sure that we can initiate recovery
	TOTAL_INST_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid) from $CONFIG_TABLE where content<>-1;"`
	ERROR_CHK $? "obtain total segement count from master instance" 2
	if [ $FAILED_INST_COUNT -gt $TOTAL_INST_COUNT ]; then
		LOG_MSG "[FATAL]:-Insufficient remaining active instances to be able to recover instance"
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	else
		LOG_MSG "[INFO]:-Sufficient active instances to be able to start recovery"
	fi
	#Now obtain details of failed instances and also their associated mirrors
	LOG_MSG "[INFO]:-Obtaining recovery information"
	ARRAY_INDEX=0
	for DBID in ${FAILED_INST_LIST[@]}
	do
		FAILED_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid, content from $CONFIG_TABLE where dbid=${DBID};"`
		ERROR_CHK $? "obtain failed segment details from master instance" 2
		ACTIVE_DBID=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select dbid from $MPP_PG_VIEW where content in (select content from $MPP_PG_VIEW where dbid=${DBID}) and dbid<>${DBID};"`
		ACTIVE_INSTANCE_ARRAY[$ARRAY_INDEX]=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid from $CONFIG_TABLE where valid='t' and content in (select content from $CONFIG_TABLE where dbid=${ACTIVE_DBID}) and dbid<>${DBID};"`
		ERROR_CHK $? "obtain recovery segment details from master instance" 2
		((ARRAY_INDEX=$ARRAY_INDEX+1))
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP instance recovery parameters"
	LOG_MSG "[INFO]:---------------------------------"
	LOG_MSG "[INFO]:-Fault tolerance setting     = $FAIL_TYPE"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		((REC_COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Recovery $REC_COUNT of $ARRAY_INDEX"
		LOG_MSG "[INFO]:------------------------------"
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		MIR_SET_VAR ${ACTIVE_INSTANCE_ARRAY[$COUNT]}
		LOG_MSG "[INFO]:-Failed instance host        = $FAIL_QE_HOST"
		LOG_MSG "[INFO]:-Failed instance directory   = $FAIL_QE_DIR"
		LOG_MSG "[INFO]:-Failed instance port        = $FAIL_QE_PORT"
		LOG_MSG "[INFO]:-Recovery instance host      = $MIR_QE_HOST"
		LOG_MSG "[INFO]:-Recovery instance directory = $MIR_QE_DIR"
		LOG_MSG "[INFO]:-Recovery instance port      = $MIR_QE_PORT"
		LOG_MSG "[INFO]:------------------------------"
		((COUNT=$COUNT+1))
	done
	$ECHO
	if [ $STOPDB -eq 1 ]; then
		LOG_MSG "[INFO]:-Procedure will STOP database as mpp_fault_action=$FAIL_TYPE"
	else
		if [ $FORCESTOP -eq 1 ];then
			LOG_MSG "[INFO]:-Procedure will STOP database as -f option supplied"
		else
			LOG_MSG "[INFO]:-Procedure can continue without database interruption"
			LOG_MSG "[INFO]:-as mpp_fault_action=$FAIL_TYPE"
		fi
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
	FAIL_QE_CONTENTID=`$ECHO $1|$CUT -d"|" -f5`
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
	if [ $STOPDB -eq 1 ] || [ $FORCESTOP -ne 0 ]; then
		CHK_FILE $MPPSTOP
        	if [ $EXISTS -ne 0 ];then
                	ERROR_EXIT "[FATAL]:-Could not locate $MPPSTOP script"
		fi
        	LOG_MSG "[INFO]:-Shutting down the instance ... Please wait ..."
        	$MPPSTOP -zfa
		if [ x"" == x"$FORCE_RECOVER_STRING" ];then
        		ERROR_CHK $? "shutdown master instance" 2
		else
			LOG_MSG "[INFO]:-Ignoring shutdown errors as force option selected"
		fi
        	LOG_MSG "[INFO]:-Successfully shutdown the MPP instance"
	else
		COUNT=0
		while [ $COUNT -lt $FAILED_INST_COUNT ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Forcing shutdown of $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m immediate -l ${FAIL_QE_DIR}.log"
			ERROR_CHK $? "force shutdown of $FAIL_QE_DIR on $FAIL_QE_HOST" 1
			LOG_MSG "[INFO]:-Checking for postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT"
			GET_PG_PID_ACTIVE $FAIL_QE_PORT $FAIL_QE_HOST
			if [ $PID -ne 0 ];then
				LOG_MSG "[WARN]:-Found postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT"
				LOG_MSG "[WARN]:-Will kill process $PID"
				$TRUSTED_SHELL $FAIL_QE_HOST "$KILL -9 $PID"
				ERROR_CHK $? "$TRUSTED_SHELL $FAIL_QE_HOST $KILL -9 $PID" 1
			else
				LOG_MSG "[INFO]:-No postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT"
			fi
			LOG_MSG "[INFO]:-Clearing /tmp/lock files for $FAIL_QE_HOST for port=$FAIL_QE_PORT "
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock /tmp/.s.PGSQL.${FAIL_QE_PORT}"
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
		if [ $STOPDB -eq 0 ] && [ $FORCESTOP -eq 0 ]; then
			LOG_MSG "[INFO]:-Issuing checkpoint on $MIR_QE_HOST port=$MIR_QE_PORT"
			$TRUSTED_SHELL $MIR_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MIR_QE_PORT $DEFAULTDB -c \"checkpoint;\""
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
		if [ $STOPDB -eq 0 ] && [ $FORCESTOP -eq 0 ]; then
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
		$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -w -D $FAIL_QE_DIR -l ${FAIL_QE_DIR}.log -o \"-i -p $FAIL_QE_PORT \" start" 
		ERROR_CHK $? "segment start $FAIL_QE_HOST instance $FAIL_QE_DIR port=$FAIL_QE_PORT" 2
		# Update table
		$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $FAIL_QE_PORT $DEFAULTDB -c \"update $MPP_MPP_TABLE set dbid=${FAIL_QE_DBID} where dbid<>${FAIL_QE_DBID};\""
		ERROR_CHK $? "update segment $MPP_MPP_TABLE on $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT" 2
		if [ $STOPDB -eq 1 ] || [ $FORCESTOP -eq 1 ]; then
				LOG_MSG "[INFO]:-Commencing shutdown of $FAIL_QE_HOST port=$FAIL_QE_PORT"
				$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m smart -l ${FAIL_QE_DIR}.log"
				ERROR_CHK $? "segment instance stop $FAIL_QE_HOST port=$FAIL_QE_PORT" 2
		fi
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RESET_QE_CONNECT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	# Obtain a user table that we can issue a select count(*) from QD to force a QE connection re-establishment
	TABLE_NAME=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select relname from pg_class where relkind='r' order by reltype desc limit(1);"`
	$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from ${TABLE_NAME};" >> $LOG_FILE 2>&1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_STATUS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COUNT=0
		LOG_MSG "[INFO]:-Starting master database in utility mode"
		$MPPSTART -m
		while [ $COUNT -lt $ARRAY_INDEX ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Setting $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT as active"
			env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c "BEGIN; update mpp_configuration set valid = 't' where dbid = CAST($FAIL_QE_DBID AS smallint); update mpp_configuration set isprimary = definedprimary where content = CAST($FAIL_QE_CONTENTID AS smallint); COMMIT;" > /dev/null 2>&1
			RETVAL=$?
			ERROR_CHK $RETVAL "update status on Master database" 2
			((COUNT=$COUNT+1))
		done
		LOG_MSG "[INFO]:-Shutting down master database in utility mode"
		$MPPSTOP -m
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_DATABASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Starting up the MPP instance in admin mode... Please wait..."
	$MPPSTART -m
	ERROR_CHK $? "start MPP instance in admin mode" 2
	LOG_MSG "[INFO]:-Resetting invalid segment status values"
	for DBID in ${FAILED_INST_LIST[@]}
	do
		env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"update $CONFIG_TABLE set valid='t' where dbid=${DBID}"
		ERROR_CHK $? "reset dbid $DBID as valid" 1
	done
	LOG_MSG "[INFO]:-Commencing shutdown of the MPP instance from admin mode... Please wait..."
	$MPPSTOP -m
	ERROR_CHK $? "stop MPP instance from admin mode" 2
        LOG_MSG "[INFO]:-Starting up the MPP instance... Please wait ..."
        $MPPSTART -a
        RETVAL=$?
        if [ $RETVAL -ne 0 ];then
		LOG_MSG "[WARN]:-The $MPPSTART script returned a non zero return code"
		LOG_MSG "[INFO]:-Check the $MPPSTART log file for more information on cause"
		LOG_MSG "[INFO]:-Run mppstate.sh to check current status of instance"
                ERROR_EXIT "[FATAL]:-Issue with master database startup." 2
	fi
        LOG_MSG "[INFO]:-Successfully started the master instance"
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
	FAILED_INSTANCE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from $CONFIG_TABLE where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`
	ERROR_CHK $? "obtain failed instance count" 2
	if [ $FAILED_INSTANCE_COUNT -eq 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	if [ $FAILED_INSTANCE_COUNT -gt 1 ];then
		ERROR_EXIT "[FATAL]:-Found multiple records for $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	FAILED_INSTANCE_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid, content from $CONFIG_TABLE where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`)
	ERROR_CHK $? "obtain failed segment instance details" 2
	FAIL_SET_VAR "${FAILED_INSTANCE_ARRAY[@]}" 
	#Get active mirror details
	ACTIVE_INSTANCE_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid from $CONFIG_TABLE where dbid<>$FAIL_QE_DBID and content in (select content from $CONFIG_TABLE where dbid=${FAIL_QE_DBID});"`)
	ERROR_CHK $? "obtain recovery segment instance details" 2
	if [ ${#ACTIVE_INSTANCE_ARRAY[@]} -ne 1 ];then
		ERROR_EXIT "[FATAL]:-Did not locate 1 recovery segment instance, obtained  ${#ACTIVE_INSTANCE_ARRAY[@]}" 2
	fi
	MIR_SET_VAR "${ACTIVE_INSTANCE_ARRAY[@]}"	
	ARRAY_INDEX=1
	STOPDB=1
	if [ $INTERACTIVE ]; then
		FAIL_TYPE="Forced Recovery -z option supplied"
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
CHK_DB_QD_SET
CHK_ON_PASSIVE_STANDBY
GET_SETTING
READ_CHK
if [ $READ_COUNT -ne 0 ];then
        ERROR_EXIT "[FATAL]:-This script cannot be used on a Master read only segment instance" 2
fi
GET_MIRROR
if [ $INTERACTIVE ]; then
	DISPLAY_PARAM
fi
TEST_ACCESS
STOP_DATABASE
RECOVER_DATA
UPDATE_MPP_MPP
if [ $STOPDB -eq 1 ] || [ $FORCESTOP -ne 0 ];then
	UPDATE_STATUS
	START_DATABASE
else
	RESET_QE_CONNECT
fi
LOG_MSG "[INFO]:-Recovery completed successfully"
LOG_MSG "[INFO]:-End MAIN"

