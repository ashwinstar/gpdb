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
FUNCTIONS=$WORKDIR/gp_bash_fuctions.sh
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
TMP_FILE=/tmp/${PROG_NAME}_${CUR_DATE}.$$
PARALLEL_STATUS_FILE=/tmp/mppstate_parallel_status_file.$$
MPPSEGREC=mppsegrec.sh
BATCH_DEFAULT=4 #Specific limit for segment recovery
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
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO 
		$ECHO "      Recovery options:"
		$ECHO "      -p <hostname> recover to spare pool node"
		$ECHO "         the pool host must have the following characteristics:"
		$ECHO "         1. Same OS version"
		$ECHO "         2. Same locale"
		$ECHO "         3. Correct Bizgres MPP software version"
		$ECHO "         4. Inter-ssh access to active MPP array"
		$ECHO "         5. MPP superuser account configured"
		$ECHO "         6. Sufficient capacity left to accommodate recovered segments"
		$ECHO "      -s <Primary segment directory,Mirror segment directory>"
		$ECHO "         example -p /data_p4/pri,/data_m4/mir, this assumes that the MPP array was"
		$ECHO "         configured with spare pool segment instance directories on each segment"
		$ECHO "         instance host"
		$ECHO "      -f, force MPP instance shutdown and restart"
        	$ECHO "      -z, <failed_segment_instance_directory_name:failed_hostname> force a recovery,"
		$ECHO "          even though no instance segments are marked as invalid on the Master"
		$ECHO "          CAUTION:-USE THIS OPTION WITH CARE!"
        	$ECHO "          example $0 -z /dbfast1/segment1:localhost"
		$ECHO "      -B, <number> run this batch of recovery segment processes in parallel [default $BATCH_DEFAULT]"
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
	FAIL_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"show gp_fault_action;"` >> $LOG_FILE 2>&1
	RET_STATUS=$?
	ERROR_CHK $RET_STATUS "obtain gp_fault_action setting" 1
	if [ $RET_STATUS -ne 0 ];then
		#If get an error, master segment instance may be down, try to start in admin mode
		$MPPSTART -maq
		ERROR_CHK $? "start master database in admin mode" 2
		#Get a list of dbid's that are marked as invalid
		CHK_MIRRORS_CONFIGURED
		if [ $MIRROR_COUNT -eq 1 ];then
			LOG_MSG "[FATAL]:-There are no mirrors configured for this MPP instance" 1
			LOG_MSG "[FATAL]:-The failed segment(s) with dbid's ${FAILED_INST_LIST[@]} will" 1
			LOG_MSG "[FATAL]:-need to be recovered manually, or a complete database restore" 1
			LOG_MSG "[FATAL]:-will need to be undertaken" 1
			$MPPSTOP -maq
			ERROR_CHK $? "shutdown master database from admin mode" 2
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		else
			LOG_MSG "[WARN]:-Unable to establish if any instances marked as invalid" 1
			LOG_MSG "[WARN]:-as master database started in admin mode" 1
			LOG_MSG "[INFO]:-Restart the database and re-run this utility to check for failures" 1
			LOG_MSG "[INFO]:-Commencing shutdown from admin mode" 1
			$MPPSTOP -maq
			ERROR_CHK $? "shutdown master database from admin mode" 2
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		fi
	else
		LOG_MSG "[INFO]:-Fault tolerance is set to $FAIL_TYPE" 1
		CHK_MIRRORS_CONFIGURED
		if [ $MIRROR_COUNT -eq 1 ];then
			LOG_MSG "[FATAL]:-There are no mirrors configured for this MPP instance" 1
			LOG_MSG "[FATAL]:-The MPP instance will need to be recovered manually" 1
			LOG_MSG "[FATAL]:-See gp_restore command" 1
			ERROR_EXIT "[FATAL]:-No further progress can be made" 2
		fi
		FAILED_INST_LIST=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where valid='f';"`)
		RET_STATUS=$?
		ERROR_CHK $RET_STATUS "obtain failed instance dbid's" 1
		LOG_MSG "[INFO]:-There are ${#FAILED_INST_LIST[@]} failed segment(s)" 1
		if [ $FAIL_TYPE != "readonly" ] && [ $FAIL_TYPE != "continue" ];then
			LOG_MSG "[FATAL]:-Parameter gp_fault_action is not set to either readonly or continue" 1
			LOG_MSG "[FATAL]:-MPP fault tolerance is not set" 1
			LOG_MSG "[FATAL]:-Unable to determine if there are actual segment failures" 1
			INFO_TEXT
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		else
			case $FAIL_TYPE in
				readonly ) STOPDB=0 
					   LOG_MSG "[INFO]:-Setting STOPDB to false" 1 ;;
				continue ) STOPDB=1
					   LOG_MSG "[INFO]:-Setting STOPDB to true" 1 ;;
			esac
		fi
	fi
	GET_MIRROR_TYPE
	if [ $MIR_TYPE_NUM -eq 3 ] && [ x"" != x"$POOL_HOST" ];then
	#Mirror type is separate hosts, check to see if pool host is active
		TYPE_ACTIVE_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select distinct(definedprimary) from $CONFIG_TABLE where hostname='${POOL_HOST}';"`)
		ERROR_CHK $? "obtain pool host active type" 2
		COUNT_TYPE_ACTIVE_ARRAY=${#TYPE_ACTIVE_ARRAY[@]}
		case $COUNT_TYPE_ACTIVE_ARRAY in
			0)
			LOG_MSG "[INFO]:-Mirror type is separate hosts, no active segments on pool host" 1
			;;
			1)
			LOG_MSG "[INFO]:-Mirror type is separate hosts, active type on pool host = ${TYPE_ACTIVE_ARRAY[@]}" 1
			PRI_COUNT=0;MIR_COUNT=0
			for DBID in ${FAILED_INST_LIST[@]}
			do
				CHK_FAIL_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select definedprimary from $CONFIG_TABLE where dbid=${DBID};"`
				ERROR_CHK $? "obtain failed segment type" 1
				case $CHK_FAIL_TYPE in
					t) ((PRI_COUNT=$PRI_COUNT+1))
					   ;;
					f) ((MIR_COUNT=$MIR_COUNT+1))
					   ;;
				esac
			done
			if [ ${TYPE_ACTIVE_ARRAY[@]} == 't' ] && [ $MIR_COUNT -ne 0 ];then
				LOG_MSG "[INFO]:-****************************************************************" 1
				LOG_MSG "[WARN]:-About to locate failed mirror to pool host with primary segments" 1
				LOG_MSG "[WARN]:-Current array mirror type is separate hosts" 1
				LOG_MSG "[WARN]:-Recovery will break this mirror configuration" 1
				LOG_MSG "[INFO]:-****************************************************************" 1
				EXIT_STATUS=1
			fi
			if [ ${TYPE_ACTIVE_ARRAY[@]} == 'f' ] && [ $PRI_COUNT -ne 0 ];then
				LOG_MSG "[INFO]:-****************************************************************" 1
				LOG_MSG "[WARN]:-About to locate failed primary to pool host with mirror segments" 1
				LOG_MSG "[WARN]:-Current array mirror type is separate hosts" 1
				LOG_MSG "[WARN]:-Recovery will break this mirror configuration" 1
				LOG_MSG "[INFO]:-****************************************************************" 1
				EXIT_STATUS=1
			fi
			;;
			2)
			LOG_MSG "[WARN]:-Mirror type is separate hosts, active type on pool host = ${TYPE_ACTIVE_ARRAY[@]}" 1
			LOG_MSG "[WARN]:-Have primary and mirror segment instances active on pool host" 1
			EXIT_STATUS=1
			;;
		esac
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

INFO_TEXT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-To be able to progress this issue, complete the following steps" 1
	LOG_MSG "[INFO]:-Step 1 Shutdown the MPP instance with mppstop.sh -zfa" 1
	LOG_MSG "[INFO]:-Step 2 Edit the Master postgresql.conf file and set gp_fault_action=readonly" 1
	LOG_MSG "[INFO]:-Step 3 Restart the MPP instance mppstart.sh -a" 1
	LOG_MSG "[INFO]:-Step 4 Re-run this utility" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_MIRROR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	FAILED_INST_COUNT=${#FAILED_INST_LIST[@]}
	if [ $FAILED_INST_COUNT -eq 0 ]; then
		LOG_MSG "[INFO]:-No invalid segment instances located" 1
		LOG_MSG "[INFO]:-Commencing general user database access check" 1
		$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"show gp_fault_action;" >> $LOG_FILE 2>&1
		RET_STATUS=$?
		if [ $RET_STATUS -ne 0 ];then
			LOG_MSG "[WARN]:-Issue with general access to the database" 1
			if [ $FAIL_TYPE != "readonly" ];then
				LOG_MSG "[INFO]:-Fault tolerance is set to $FAIL_TYPE" 1
				LOG_MSG "[INFO]:-Please review the following" 1
				INFO_TEXT
				ERROR_EXIT "[FATAL]:-Unable to continue" 2
			fi
		else
			LOG_MSG "[INFO]:-General access checked passed" 1
		fi
		exit 0
	else
		LOG_MSG "[INFO]:-Located $FAILED_INST_COUNT invalid segment instances" 1
	fi
	#Now check to make sure that we can initiate recovery
	TOTAL_INST_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid) from $CONFIG_TABLE where content<>-1;"`
	ERROR_CHK $? "obtain total segement count from master instance" 2
	if [ $FAILED_INST_COUNT -gt $TOTAL_INST_COUNT ]; then
		LOG_MSG "[FATAL]:-Insufficient remaining active instances to be able to recover instance" 1
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	else
		LOG_MSG "[INFO]:-Sufficient active instances to be able to start recovery" 1
	fi
	#Now obtain details of failed instances and also their associated mirrors
	LOG_MSG "[INFO]:-Obtaining recovery information" 1
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
	LOG_MSG "[INFO]:-MPP instance recovery parameters" 1
	LOG_MSG "[INFO]:---------------------------------" 1
	LOG_MSG "[INFO]:-Fault tolerance setting       = $FAIL_TYPE" 1
	if [ x"" != x"$POOL_HOST" ];then
	LOG_MSG "[INFO]:-Recovery type                 = Pool Host" 1
	LOG_MSG "[INFO]:-Pool host for recovery        = $POOL_HOST" 1
	TYPE_TEXT="Pool    "
	fi
	if [ x"" != x"$POOL_DIR_SET" ];then
	LOG_MSG "[INFO]:-Recovery type                 = Pool Directory" 1
	LOG_MSG "[INFO]:-Primary pool directory        = $PRI_POOL_DATADIR" 1
	LOG_MSG "[INFO]:-Mirror pool directory         = $MIR_POOL_DATADIR" 1
	TYPE_TEXT="Pool dir"
	fi
	if [ x"" == x"$POOL_HOST" ] && [ x"" == x"$POOL_DIR_SET" ];then
	LOG_MSG "[INFO]:-Recovery type                 = Standard" 1
	TYPE_TEXT="Failed  "
	fi
	COUNT=0
	LOG_MSG "[INFO]:-Recovery parallel batch value = $BATCH_DEFAULT" 1
	if [ $EXIT_STATUS -ne 0 ];then
	LOG_MSG "[WARN]:-Warnings have been generated    <<<<<<<<<<<<<" 1
	LOG_MSG "[INFO]:-But recovery can continue" 1
	fi
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		((REC_COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Recovery $REC_COUNT of $ARRAY_INDEX" 1
		LOG_MSG "[INFO]:------------------------------" 1
		if [ x"" != x"$POOL_DIR_SET" ] || [ x"" != x"$POOL_HOST" ];then
			FAIL_SET_VAR ${POOL_FAILED_INSTANCE_ARRAY[$COUNT]}
		else
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		fi
		MIR_SET_VAR ${ACTIVE_INSTANCE_ARRAY[$COUNT]}
		LOG_MSG "[INFO]:-$TYPE_TEXT instance host        = $FAIL_QE_HOST" 1
		LOG_MSG "[INFO]:-$TYPE_TEXT instance directory   = $FAIL_QE_DIR" 1
		LOG_MSG "[INFO]:-$TYPE_TEXT instance port        = $FAIL_QE_PORT" 1
		LOG_MSG "[INFO]:-Recovery instance host        = $MIR_QE_HOST" 1
		LOG_MSG "[INFO]:-Recovery instance directory   = $MIR_QE_DIR" 1
		LOG_MSG "[INFO]:-Recovery instance port        = $MIR_QE_PORT" 1
		LOG_MSG "[INFO]:------------------------------" 1
		((COUNT=$COUNT+1))
	done
	$ECHO
	if [ $STOPDB -eq 1 ]; then
		LOG_MSG "[INFO]:-Procedure will STOP database as gp_fault_action=$FAIL_TYPE" 1
	else
		if [ $FORCESTOP -eq 1 ];then
			LOG_MSG "[INFO]:-Procedure will STOP database as -f option supplied" 1
		else
			LOG_MSG "[INFO]:-Procedure can continue without database interruption" 1
			LOG_MSG "[INFO]:-as gp_fault_action=$FAIL_TYPE" 1
		fi
	fi
	$ECHO
        $ECHO "Continue with recovery procedure yY|nN >"
        read REPLY
	if [ x"" == x"$REPLY" ];then
		LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
		$RM -f $TMP_FILE
		exit 1;fi
        if [ $REPLY != Y ] && [ $REPLY != y ];then
                LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
		$RM -f $TMP_FILE
                exit 1;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

FAIL_SET_VAR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	FAIL_QE_HOST=`$ECHO $1|$CUT -d"|" -f1`
	FAIL_QE_PORT=`$ECHO $1|$CUT -d"|" -f2`
	FAIL_QE_DIR=`$ECHO $1|$CUT -d"|" -f3`
	FAIL_QE_DBID=`$ECHO $1|$CUT -d"|" -f4`
	FAIL_QE_CONTENTID=`$ECHO $1|$CUT -d"|" -f5`
	FAIL_QE_TOP_DIR=`$DIRNAME $FAIL_QE_DIR`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

MIR_SET_VAR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MIR_QE_HOST=`$ECHO $1|$CUT -d"|" -f1`
	MIR_QE_PORT=`$ECHO $1|$CUT -d"|" -f2`
	MIR_QE_DIR=`$ECHO $1|$CUT -d"|" -f3`
	MIR_QE_DBID=`$ECHO $1|$CUT -d"|" -f4`
	MIR_QE_TOP_DIR=`$DIRNAME $MIR_QE_DIR`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

TEST_ACCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Testing remote access" 1
	LOG_MSG "[INFO]:-Trusted shell = $TRUSTED_SHELL" 1
	LOG_MSG "[IMFO]:-Trusted copy  = $TRUSTED_COPY" 1
	LOG_MSG "[INFO]:-Please wait..." 1
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
	LOG_MSG "[INFO]:-Remote access checks completed" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
STOP_DATABASE () { 
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $STOPDB -eq 1 ] || [ $FORCESTOP -ne 0 ]; then
		CHK_FILE $MPPSTOP
        	if [ $EXISTS -ne 0 ];then
                	ERROR_EXIT "[FATAL]:-Could not locate $MPPSTOP script"
		fi
        	LOG_MSG "[INFO]:-Shutting down the instance ... Please wait ..." 1
        	$MPPSTOP -zfa
		if [ x"" == x"$FORCE_RECOVER_STRING" ];then
        		ERROR_CHK $? "shutdown master instance" 2
		else
			LOG_MSG "[INFO]:-Ignoring shutdown errors as force option selected" 1
		fi
        	LOG_MSG "[INFO]:-Successfully shutdown the MPP instance" 1
	else
		COUNT=0
		while [ $COUNT -lt $FAILED_INST_COUNT ]
		do
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
			LOG_MSG "[INFO]:-Forcing shutdown of $FAIL_QE_HOST for port=$FAIL_QE_PORT " 1
			$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m immediate -l ${FAIL_QE_DIR}.log 2>/dev/null"
			ERROR_CHK $? "force shutdown of $FAIL_QE_DIR on $FAIL_QE_HOST" 1
			LOG_MSG "[INFO]:-Checking for postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT" 1
			GET_PG_PID_ACTIVE $FAIL_QE_PORT $FAIL_QE_HOST
			if [ $PID -ne 0 ];then
				LOG_MSG "[WARN]:-Found postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT" 1
				LOG_MSG "[WARN]:-Will kill process $PID" 1
				$TRUSTED_SHELL $FAIL_QE_HOST "$KILL -9 $PID 2>/dev/null"
				ERROR_CHK $? "$TRUSTED_SHELL $FAIL_QE_HOST $KILL -9 $PID" 1
			else
				LOG_MSG "[INFO]:-No postmaster process on $FAIL_QE_HOST port $FAIL_QE_PORT" 1
			fi
			LOG_MSG "[INFO]:-Clearing /tmp/lock files for $FAIL_QE_HOST for port=$FAIL_QE_PORT " 1
			$TRUSTED_SHELL $FAIL_QE_HOST "$RM -f /tmp/.s.PGSQL.${FAIL_QE_PORT}.lock /tmp/.s.PGSQL.${FAIL_QE_PORT}"
			ERROR_CHK $? "remove of /tmp lockfiles for $FAIL_QE_HOST" 1
			
			((COUNT=$COUNT+1))		
		done
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RECOVER_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PARALLEL_SETUP  $PARALLEL_STATUS_FILE
	BATCH_LIMIT=${#FAILED_INSTANCE_ARRAY[@]}
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		MIR_SET_VAR ${ACTIVE_INSTANCE_ARRAY[$COUNT]}
		if [ x"" != x"$POOL_DIR_SET" ] || [ x"" != x"$POOL_HOST" ];then
			POOL_ARRAY=${POOL_FAILED_INSTANCE_ARRAY[$COUNT]}
		else
			POOL_ARRAY=0
		fi
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
		$MPPSEGREC $$ $FAIL_QE_HOST $FAIL_QE_PORT $FAIL_QE_DIR $FAIL_QE_DBID $FAIL_QE_CONTENTID $FAIL_QE_TOP_DIR \
 $MIR_QE_HOST $MIR_QE_PORT $MIR_QE_DIR $MIR_QE_DBID $MIR_QE_TOP_DIR $INST_COUNT $LOG_FILE $STOPDB $FORCESTOP $POOL_ARRAY &
		PARALLEL_COUNT $BATCH_LIMIT $BATCH_DEFAULT 
		((COUNT=$COUNT+1))
	done
	PARALLEL_SUMMARY_STATUS_REPORT $PARALLEL_STATUS_FILE
	if [ $FAILED_COUNT -ne 0 ] || [ $KILLED_COUNT -ne 0 ];then
		LOG_MSG "[FATAL]:-Parallel processes report failure(s), check log file" 1
		ERROR_EXIT "[FATAL]:-Unable to continue, script exits" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_MPP_MPP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	COUNT=0
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		if [ x"" != x"$POOL_DIR_SET" ] || [ x"" != x"$POOL_HOST" ];then
			FAIL_SET_VAR ${POOL_FAILED_INSTANCE_ARRAY[$COUNT]}
		else
			FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		fi
		((COUNT=$COUNT+1))
		LOG_MSG "[INFO]:-Updating instance catalog on $FAIL_QE_HOST port=$FAIL_QE_PORT" 1
		LOG_MSG "[INFO]:-Starting instance on $FAIL_QE_HOST port=$FAIL_QE_PORT" 1
		$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -w -D $FAIL_QE_DIR -l ${FAIL_QE_DIR}.log -o \"-i -p $FAIL_QE_PORT \" start" 
		ERROR_CHK $? "segment start $FAIL_QE_HOST instance $FAIL_QE_DIR port=$FAIL_QE_PORT" 2
		# Update table
		$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $FAIL_QE_PORT $DEFAULTDB -c \"update $MPP_MPP_TABLE set dbid=${FAIL_QE_DBID} where dbid<>${FAIL_QE_DBID};\""
		ERROR_CHK $? "update segment $MPP_MPP_TABLE on $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT" 2
		if [ $STOPDB -eq 1 ] || [ $FORCESTOP -eq 1 ]; then
				LOG_MSG "[INFO]:-Commencing shutdown of $FAIL_QE_HOST port=$FAIL_QE_PORT" 1
				$TRUSTED_SHELL $FAIL_QE_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -W stop -D $FAIL_QE_DIR -m smart -l ${FAIL_QE_DIR}.log"
				ERROR_CHK $? "segment instance stop $FAIL_QE_HOST port=$FAIL_QE_PORT" 2
		fi
	done
	if [ x"" != x"$POOL_DIR_SET" ] || [ x"" != x"$POOL_HOST" ];then
		POOL_CATALOG_UPDATE	
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RESET_QE_CONNECT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	TABLE_NAME=`$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select relname from pg_class where relkind='r' order by reltype desc limit(1);"`
	$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from ${TABLE_NAME};" >> $LOG_FILE 2>&1
	$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select * from ${MPP_PG_VIEW};" >> $LOG_FILE 2>&1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_DATABASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Starting up the MPP instance in admin mode... Please wait..." 1
	$MPPSTART -m
	ERROR_CHK $? "start MPP instance in admin mode" 2
	LOG_MSG "[INFO]:-Resetting invalid segment status values" 1
	for DBID in ${FAILED_INST_LIST[@]}
	do
		env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"update $CONFIG_TABLE set valid='t' where dbid=${DBID}"
		ERROR_CHK $? "reset dbid $DBID as valid" 1
	done
	LOG_MSG "[INFO]:-Commencing shutdown of the MPP instance from admin mode... Please wait..." 1
	$MPPSTOP -m
	ERROR_CHK $? "stop MPP instance from admin mode" 2
        LOG_MSG "[INFO]:-Starting up the MPP instance... Please wait..." 1
        $MPPSTART -a
        RETVAL=$?
        if [ $RETVAL -ne 0 ];then
		LOG_MSG "[WARN]:-The $MPPSTART script returned a non zero return code" 1
		LOG_MSG "[INFO]:-Check the $MPPSTART log file for more information on cause" 1
		LOG_MSG "[INFO]:-Run mppstate.sh to check current status of instance" 1
                ERROR_EXIT "[FATAL]:-Issue with master database startup." 2
	fi
        LOG_MSG "[INFO]:-Successfully started the master instance" 1
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
	FAILED_INSTANCE_COUNT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from $CONFIG_TABLE where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`
	ERROR_CHK $? "obtain failed instance count" 2
	if [ $FAILED_INSTANCE_COUNT -eq 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	if [ $FAILED_INSTANCE_COUNT -gt 1 ];then
		ERROR_EXIT "[FATAL]:-Found multiple records for $FAIL_QE_DIR for host $FAIL_QE_HOST" 2;fi
	FAILED_INSTANCE_ARRAY=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid, content from $CONFIG_TABLE where datadir='${FAIL_QE_DIR}' and hostname='${FAIL_QE_HOST}';"`)
	ERROR_CHK $? "obtain failed segment instance details" 2
	FAIL_SET_VAR "${FAILED_INSTANCE_ARRAY[@]}" 
	FAILED_INST_LIST=($FAIL_QE_DBID)
	#Get active mirror details
	ACTIVE_INSTANCE_ARRAY=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid from $CONFIG_TABLE where dbid<>$FAIL_QE_DBID and content in (select content from $CONFIG_TABLE where dbid=${FAIL_QE_DBID});"`)
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

VALIDATE_PARAMS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ x"" != x"$POOL_HOST" ] && [ x"" != x"$POOL_DIR_SET" ];then
		ERROR_EXIT "[FATAL]:-Cannot supply -s and -p options at same time" 2
	fi
	if [ x"" != x"$POOL_HOST" ];then
		if [ `$HOSTNAME` == $POOL_HOST ];then
			ERROR_EXIT "[FATAL]:-Pool host cannot be Master instance hostname" 2
		fi
	fi
	if [ x"" != x"$POOL_DIR_SET" ];then
		if [ `$ECHO $POOL_DIR_SET|$GREP -c ','` -ne 1 ];then
			ERROR_EXIT "[FATAL]:Expected dir,dir for -s option" 2
		fi
		PRI_POOL_DATADIR=`$ECHO $POOL_DIR_SET|$CUT -d"," -f1|$SED -e 's/\/$//g'`
		if [ x"`$ECHO $PRI_POOL_DATADIR|$AWK -F"/" '{print $1}'`" != x"" ];then
			ERROR_EXIT "[FATAL]:Primary directory not prefixed by /" 2
		fi
		MIR_POOL_DATADIR=`$ECHO $POOL_DIR_SET|$CUT -d"," -f2|$SED -e 's/\/$//g'`
		if [ x"`$ECHO $MIR_POOL_DATADIR|$AWK -F"/" '{print $1}'`" != x"" ];then
			ERROR_EXIT "[FATAL]:Mirror directory not prefixed by /" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_POOL_DIR_AVAILABLE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Primary segment pool directory set as $PRI_POOL_DATADIR" 1
	LOG_MSG "[INFO]:-Mirror segment pool directory set as $MIR_POOL_DATADIR" 1
	SEG_PREFIX=`$ECHO $MASTER_DATA_DIRECTORY|$AWK -F'/' '{print $NF}'|$SED -e 's/-[0-9]//g'`
	LOG_MSG "[INFO]:-MPP instance segment prefix is $SEG_PREFIX" 1
	#For each host see how many are active
	ACTIVE_PRI_POOL_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid from $CONFIG_TABLE where datadir like '${PRI_POOL_DATADIR}/${SEG_PREFIX}%' and content<>-1;"`)
	ERROR_CHK $? "obtain active primary pool directory count" 2
	LOG_MSG "[INFO]:-There are ${#ACTIVE_PRI_POOL_ARRAY[@]} primary directory pools active" 1
	ACTIVE_MIR_POOL_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname, port, datadir, dbid from $CONFIG_TABLE where datadir like '${MIR_POOL_DATADIR}/${SEG_PREFIX}%' and content<>-1;"`)
	ERROR_CHK $? "obtain active mirror pool directory count" 2
	LOG_MSG "[INFO]:-There are ${#ACTIVE_MIR_POOL_ARRAY[@]} mirror directory pools active" 1
	#Maximum pool directory usage will be defined by number of primary QE hosts in the array
	POOL_DIR_LIMIT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(distinct(hostname)) from $CONFIG_TABLE where content<>-1 and definedprimary='t';"`
	ERROR_CHK $? "obtain pool directory maximum limit" 2
	LOG_MSG "[INFO]:-Maximum pool usage limit is $POOL_DIR_LIMIT for this MPP instance" 1
	#Now see if we can continue
	GET_FAIL_COUNT_TYPE
	if [ $PRI_FAIL_COUNT -ne 0 ];then
	#Check to see if have a spare primary pool directory
		((PRI_FREE=$POOL_DIR_LIMIT-${#ACTIVE_PRI_POOL_ARRAY[@]}))
		if [ $PRI_FREE -lt $PRI_FAIL_COUNT ];then
			LOG_MSG "[FATAL]:-There are insufficient spare primary pool directories" 1
			LOG_MSG "[FATAL]:-Required=$PRI_FAIL_COUNT Free=$PRI_FREE Used=${#ACTIVE_PRI_POOL_ARRAY[@]}" 1
			LOG_MSG "[INFO]:-Consider using a pool host with -p option to this utility" 1
			LOG_MSG "[INFO]:-Or supply different directory names to -s option, if available" 1
			LOG_MSG "[INFO]:-If host with failed segment instance still online try" 1
			LOG_MSG "[INFO]:-mpprecoverseg.sh without any options to attempt recovery" 1
			LOG_MSG "[INFO]:-to original segment instance directory" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		else
			LOG_MSG "[INFO]:-Sufficient spare primary pool directories" 1
			LOG_MSG "[INFO]:-Required=$PRI_FAIL_COUNT Free=$PRI_FREE Used=${#ACTIVE_PRI_POOL_ARRAY[@]}" 1
		fi
	fi
	if [ $MIR_FAIL_COUNT -ne 0 ];then
		((MIR_FREE=$POOL_DIR_LIMIT-${#ACTIVE_MIR_POOL_ARRAY[@]}))
		if [ $MIR_FREE -lt $MIR_FAIL_COUNT ];then
			LOG_MSG "[FATAL]:-There are insufficient spare mirror pool directories" 1
			LOG_MSG "[FATAL]:-Required=$MIR_FAIL_COUNT Free=$MIR_FREE Used=${#ACTIVE_MIR_POOL_ARRAY[@]}" 1
			LOG_MSG "[INFO]:-Consider using a pool host with -p option to this utility" 1
			LOG_MSG "[INFO]:-Or supply different directory names to -s option, if available" 1
			LOG_MSG "[INFO]:-If host with failed segment instance still online try" 1
			LOG_MSG "[INFO]:-mpprecoverseg.sh without any options to attempt recovery" 1
			LOG_MSG "[INFO]:-to original segment instance directory" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		else
			LOG_MSG "[INFO]:-Sufficient spare mirror pool directories" 1
			LOG_MSG "[INFO]:-Required=$MIR_FAIL_COUNT Free=$MIR_FREE Used=${#ACTIVE_MIR_POOL_ARRAY[@]}" 1
		fi
	fi
	#Now obtain port assignment information
	PROCESS_PORT_VALUES
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_PORT_VALUES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MAX_PRI_PORT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(port) from $CONFIG_TABLE where content<>-1 and definedprimary='t';"`
	ERROR_CHK $? "obtain max primary port number" 2
	MIN_PRI_PORT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select min(port) from $CONFIG_TABLE where content<>-1 and definedprimary='t';"`
	ERROR_CHK $? "obtain min primary port number" 2
	MAX_MIR_PORT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(port) from $CONFIG_TABLE where content<>-1 and definedprimary='f';"`
	ERROR_CHK $? "obtain max mirror port number" 2	
	MIN_MIR_PORT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select min(port) from $CONFIG_TABLE where content<>-1 and definedprimary='f';"`
	ERROR_CHK $? "obtain min mirror port number" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_FAIL_COUNT_TYPE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PRI_FAIL_COUNT=0;MIR_FAIL_COUNT=0
	for FAIL_DBID in ${FAILED_INST_LIST[@]}
	do
		DBID_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select definedprimary from $MPP_PG_VIEW where dbid=${FAIL_DBID};"`
		ERROR_CHK $? "obtain type for dbid = $FAIL_DBID" 2
		case $DBID_TYPE in
			t ) ((PRI_FAIL_COUNT=$PRI_FAIL_COUNT+1)) ;;
			f ) ((MIR_FAIL_COUNT=$MIR_FAIL_COUNT+1)) ;;
		esac
	done		
	LOG_MSG "[INFO]:-Number of primary segments failed = $PRI_FAIL_COUNT" 1
	LOG_MSG "[INFO]:-Number of mirror segments failed  = $MIR_FAIL_COUNT" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

POOL_DIR_VALIDATION () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Now check to see that can use a free pool directory
	PRI_POOL_DIR_HOST_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select distinct(hostname) from $CONFIG_TABLE where content<>-1 and definedprimary='t' order by hostname;"`)
	ERROR_CHK $? "obtain primary segment hostname list" 2
	MIR_POOL_DIR_HOST_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select distinct(hostname) from $CONFIG_TABLE where content<>-1 and definedprimary='f' order by hostname;"`)
	ERROR_CHK $? "obtain mirror segment hostname list" 2
	for QE_HOST in ${PRI_POOL_DIR_HOST_ARRAY[@]}
	do
		LOG_MSG "[INFO]:-Checking access to primary host $QE_HOST" 1
		PING_HOST $QE_HOST
		if [ $RETVAL -eq 0 ];then
			PRI_POOL_DIR_HOST_CHK_ARRAY=(${PRI_POOL_DIR_HOST_CHK_ARRAY[@]} $QE_HOST)
		else
			LOG_MSG "[WARN]:-Failed to establish network contact with $QE_HOST" 1
		fi
		#Now check to ensure have pool directories
		CHK_DIR $PRI_POOL_DATADIR $QE_HOST
		if [ $EXISTS -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Directory $PRI_POOL_DATADIR does not exist on $QE_HOST" 2
		fi
	done
	for QE_HOST in ${MIR_POOL_DIR_HOST_ARRAY[@]}
	do
		LOG_MSG "[INFO]:-Checking access to mirror host $QE_HOST" 1
		PING_HOST $QE_HOST
		if [ $RETVAL -eq 0 ];then
			MIR_POOL_DIR_HOST_CHK_ARRAY=(${MIR_POOL_DIR_HOST_CHK_ARRAY[@]} $QE_HOST)
		else
			LOG_MSG "[WARN]:-Failed to establish network contact with $QE_HOST" 1
		fi
		CHK_DIR $MIR_POOL_DATADIR $QE_HOST
		if [ $EXISTS -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Directory $MIR_POOL_DATADIR does not exist on $QE_HOST" 2
		fi
	done
	#Now process the failed instance list
	#Start the update sql for the config table change
	$ECHO "BEGIN;" > $TMP_FILE
	ARRAY_VECTOR=0
	for FAIL_LINE in ${FAILED_INSTANCE_ARRAY[@]}
	do
		FAIL_SET_VAR $FAIL_LINE
		#Now check to see if this a primary or mirror segment
		SEG_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select definedprimary from $CONFIG_TABLE where dbid=${FAIL_QE_DBID};"`
		ERROR_CHK $? "get segment type" 2
		case $SEG_TYPE in 
			t ) #Primary segment
				#NEW_HOST=${PRI_POOL_DIR_HOST_CHK_ARRAY[$ARRAY_VECTOR]}
				GET_HOST 
				NEW_DIR=${PRI_POOL_DATADIR}/${SEG_PREFIX}$FAIL_QE_CONTENTID
				NEW_TOP_DIR=${PRI_POOL_DATADIR}
				CHK_FREE_PORT $NEW_HOST $MIN_PRI_PORT $MAX_PRI_PORT
				if [ $FREE_PORT -eq 0 ];then
					((NEW_PORT=$MAX_PRI_PORT+1))
				else
					NEW_PORT=$FREE_PORT
				fi
				PRI_USED=(${PRI_USED[@]} $NEW_HOST)
				;;
			f ) #Mirror_segment
				GET_HOST 
				NEW_DIR=${MIR_POOL_DATADIR}/${SEG_PREFIX}$FAIL_QE_CONTENTID
				NEW_TOP_DIR=${MIR_POOL_DATADIR}
				CHK_FREE_PORT $NEW_HOST $MIN_MIR_PORT $MAX_MIR_PORT	
				if [ $FREE_PORT -eq 0 ];then
					((NEW_PORT=$MAX_MIR_PORT+1))
				else
					NEW_PORT=$FREE_PORT
				fi
				MIR_USED=(${MIR_USED[@]} $NEW_HOST)
				;;
		esac
		$ECHO "update $CONFIG_TABLE set hostname='${NEW_HOST}', datadir='${NEW_DIR}', port=${NEW_PORT} where dbid=${FAIL_QE_DBID};" >> $TMP_FILE
		POOL_FAILED_INSTANCE_ARRAY[$ARRAY_VECTOR]="${NEW_HOST}|${NEW_PORT}|${NEW_DIR}|${FAIL_QE_DBID}|${FAIL_QE_CONTENTID}" 
		((ARRAY_VECTOR=$ARRAY_VECTOR+1))
	done	
	$ECHO "COMMIT;" >> $TMP_FILE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_HOST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Get the host that contains the primary/mirror segment for this mirror
	PARTNER_HOST=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select hostname from $CONFIG_TABLE where content=${FAIL_QE_CONTENTID} and definedprimary<>'${SEG_TYPE}';"`
	ERROR_CHK $? "obtain partner hostname" 2
	LOG_MSG "[INFO]:-Partner host for $FAIL_QE_HOST is $PARTNER_HOST" 1
	NEW_HOST=UNKNOWN
	if [ $SEG_TYPE == 't' ];then
		CHK_PARTNER_ARRAY=(${PRI_POOL_DIR_HOST_CHK_ARRAY[@]})
		DIR_CHK=$PRI_POOL_DATADIR
		USED=(${PRI_USED[@]})
	else
		CHK_PARTNER_ARRAY=(${MIR_POOL_DIR_HOST_CHK_ARRAY[@]})
		DIR_CHK=$MIR_POOL_DATADIR
		USED=(${MIR_USED[@]})
	fi
	for CHK_HOST in ${CHK_PARTNER_ARRAY[@]}
	do	
		if [ `$ECHO ${USED[@]}|$GREP -c $CHK_HOST` -eq 0 ];then
			#Hostname not used at present
			if [ $CHK_HOST != $PARTNER_HOST ];then
				#Found a host we can use, check to ensure that datadir not in use
				LOG_MSG "[INFO]:-Checking host $CHK_HOST to see if pool directory active" 1
				IS_USED=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${CHK_HOST}' and datadir like '${DIR_CHK}%';"`
				if [ $IS_USED -eq 0 ];then
					NEW_HOST=$CHK_HOST
					break
				else
					LOG_MSG "[INFO]:-Host $CHK_HOST has an active pool directory, looking for alternative" 1
				fi
			else
				LOG_MSG "[INFO]:-Possible host $CHK_HOST is a partner, checking for another alternative host" 1
			fi
		fi
	done
	if [ $NEW_HOST == "UNKNOWN" ];then
		#Have been unable to locate another hostname which is not the partner
		#Make sure that pool directory not in use
		IS_USED=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${PARTNER_HOST}' and datadir like '${DIR_CHK}%';"`
		if [ $IS_USED -eq 0 ];then
			LOG_MSG "[INFO]:**************************************************" 1
			LOG_MSG "[WARN]:-Have to locate segment on partner host $PRI_HOST" 1
			LOG_MSG "[INFO]:-If host containing failed segment instance is" 1
			LOG_MSG "[INFO]:-still active, consider use of mpprecoverseg.sh" 1
			LOG_MSG "[INFO]:-without options to attempt recovery to original" 1
			LOG_MSG "[INFO]:-segment instance directory" 1
			LOG_MSG "[INFO]:**************************************************" 1
			NEW_HOST=$PARTNER_HOST
			EXIT_STATUS=1
		else
			LOG_MSG "[FATAL]:-Unable to locate a pool directory host for recovery" 1
			LOG_MSG "[INFO]:-Consider use of mpprecoverseg.sh without any options" 1
			LOG_MSG "[INFO]:-Or use mpprecoverseg.sh -p <pool host name>" 1
			ERROR_EXIT "[FATAL]:-unable to continue" 2
		fi
	else
		LOG_MSG "[INFO]:-Identified a non-partner host $NEW_HOST" 1
	fi 
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
	
CHK_FREE_PORT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#First see if the current failed port can be used
	FREE_COUNT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${1}' and port=${FAIL_QE_PORT};"`
	ERROR_CHK $? "obtain failed port usage" 2
	if [ $FREE_COUNT -eq 0 ] && [ `$ECHO ${POOL_PORT_IN_USE[@]}|$GREP -c $FAIL_QE_PORT` -eq 0 ];then
		LOG_MSG "[INFO]:-Failed port $FAIL_QE_PORT not in use on $1, will re-use" 1
		FREE_PORT=$FAIL_QE_PORT
	else
		FREE_PORT=0;START_PORT=$2
		while [ $START_PORT -le $3 ]
		do
			FREE_COUNT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${1}' and port=${START_PORT};" `
			ERROR_CHK $? "obtain free port count" 2
			LOG_MSG "[INFO]:-Free port count for $START_PORT is $FREE_COUNT" 1
			if [ $FREE_COUNT -eq 0 ] && [ `$ECHO ${POOL_PORT_IN_USE[@]}|$GREP -c $START_PORT` -eq 0 ];then
				FREE_PORT=$START_PORT
				break
			fi
			((START_PORT=$START_PORT+1))
		done
	fi
	LOG_MSG "[INFO]:-Free port value current set at $FREE_PORT" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

POOL_CATALOG_UPDATE () {
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	if [ ! -f $TMP_FILE ];then
		ERROR_EXIT "[FATAL]:-SQL update file $TMP_FILE deleted" 2
	fi	
	LOG_MSG "[INFO]:-Dumping MPP configuration to log file" 1
	$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -c"select * from gp_configuration;" >> $LOG_FILE
	LOG_MSG "[INFO]:-Configuration dump complete" 1
	LOG_MSG "[INFO]:-Commencing system catalog update" 1
	$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -f $TMP_FILE 2>&1 > /dev/null
	ERROR_CHK $? "update system catalog" 2
	$RM -f $TMP_FILE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_POOL_HOST () {
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	#Check that the pool host is available on the network
	PING_HOST $POOL_HOST
	if [ $RETVAL -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Issue with network access to $POOL_HOST" 2
	else
		LOG_MSG "[INFO]:-Established network connection to $POOL_HOST" 1
	fi
	#Check ssh access
	LOG_MSG "[INFO]:-Checking ssh access as user $USER to $POOL_HOST" 1
	$TRUSTED_SHELL $POOL_HOST "ls /tmp > /dev/null"
	ERROR_CHK $? "ssh access check to $POOL_HOST" 2
	#Check for $MPPHOME directory on pool host
	CHK_DIR $MPPHOME $POOL_HOST
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Issue with $MPPHOME directory on $POOL_HOST" 2
	else
		LOG_MSG "[INFO]:-Check for directory $MPPHOME passed" 1
	fi
	#Check that correct software and OS versions installed
	QD_INITDB_VERSION=`$INITDB -V`
	ERROR_CHK $? "obtain master initdb version" 2
	LOG_MSG "[INFO]:-Master initdb version = $QD_INITDB_VERSION"	 1
	POOL_INITDB_VERSION=`$TRUSTED_SHELL $POOL_HOST "$INITDB -V"`
	ERROR_CHK $? "obtain pool host initdb version" 2
	LOG_MSG "[INFO]:-Pool host initdb version = $POOL_INITDB_VERSION" 1
	if [ "$QD_INITDB_VERSION" != "$POOL_INITDB_VERSION" ];then
		ERROR_EXIT "[FATAL]:-Initdb version mismatch between master and $POOL_HOST" 2
	else
		LOG_MSG "[INFO]:-Initdb version check passed" 1
	fi
	GET_FAIL_COUNT_TYPE
	#Check for directories in use
	PRI_POOL_HOST_IN_USE=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datadir from $CONFIG_TABLE where hostname='$POOL_HOST' and definedprimary='t';"`)
	ERROR_CHK $? "obtain pool host primary in use" 2
	LOG_MSG "[INFO]:-Pool host $POOL_HOST active primary segment count = ${#PRI_POOL_HOST_IN_USE[@]}" 1
	MIR_POOL_HOST_IN_USE=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datadir from $CONFIG_TABLE where hostname='$POOL_HOST' and definedprimary='f';"`)
	ERROR_CHK $? "obtain pool host mirror in use" 2
	LOG_MSG "[INFO]:-Pool host $POOL_HOST active mirror segment count  = ${#MIR_POOL_HOST_IN_USE[@]}" 1
	#Get number of primary and mirror segments on a standard host
	POOL_HOST_LIMIT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(distinct port) from $CONFIG_TABLE where definedprimary='t' and content<>-1;"`
	ERROR_CHK $? "Obtain Array segment per host" 2
	LOG_MSG "[INFO]:-Pool host segment limit is $POOL_HOST_LIMIT" 1
	((PRI_FREE=$POOL_HOST_LIMIT-${#PRI_POOL_HOST_IN_USE[@]}))	
	if [ $PRI_FREE -lt $PRI_FAIL_COUNT ];then
		LOG_MSG "[FATAL]:-There are insufficient spare primary pool directories" 1
		LOG_MSG "[FATAL]:-Required=$PRI_FAIL_COUNT Free=$PRI_FREE Used=${#PRI_POOL_HOST_IN_USE[@]}" 1
		LOG_MSG "[INFO]:-Consider using mpprecoverseg.sh with -s option" 1
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	else
		LOG_MSG "[INFO]:-Sufficient spare primary pool directories" 1
		LOG_MSG "[INFO]:-Required=$PRI_FAIL_COUNT Free=$PRI_FREE Used=${#PRI_POOL_HOST_IN_USE[@]}" 1
	fi
	((MIR_FREE=$POOL_HOST_LIMIT-${#MIR_POOL_HOST_IN_USE[@]}))	
	if [ $MIR_FREE -lt $MIR_FAIL_COUNT ];then
		LOG_MSG "[FATAL]:-There are insufficient spare mirror pool directories" 1
		LOG_MSG "[FATAL]:-Required=$MIR_FAIL_COUNT Free=$MIR_FREE Used=${#MIR_POOL_HOST_IN_USE[@]}" 1
		LOG_MSG "[INFO]:-Consider using mpprecoverseg.sh with -s option" 1
		ERROR_EXIT "[FATAL]:-Unable to continue" 2
	else
		LOG_MSG "[INFO]:-Sufficient spare mirror pool directories" 1
		LOG_MSG "[INFO]:-Required=$MIR_FAIL_COUNT Free=$MIR_FREE Used=${#MIR_POOL_HOST_IN_USE[@]}" 1
	fi
	SEG_PREFIX=`$ECHO $MASTER_DATA_DIRECTORY|$AWK -F'/' '{print $NF}'|$SED -e 's/-[0-9]//g'`
	LOG_MSG "[INFO]:-MPP instance segment prefix is $SEG_PREFIX" 1
	LOG_MSG "[INFO]:-Obtaining a list of primary segment datadir prefixes" 1
	PRI_DATA_DIR_PREFIX_ARRAY=(`for DIR in \`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datadir from $CONFIG_TABLE where content<>-1 and definedprimary='t';"\`;do $DIRNAME $DIR;done|$SORT -u|$TR '\n' ' '`)
	ERROR_CHK $? "obtain primary segment prefixes" 2
	LOG_MSG "[INFO]:-Commencing primary directory checks" 1
	for PRI_DIR in ${PRI_DATA_DIR_PREFIX_ARRAY[@]}
	do
		CHK_WRITE_DIR $PRI_DIR $POOL_HOST
	done
	LOG_MSG "[INFO]:-Obtaining a list of mirror segment datadir prefixes" 1
	MIR_DATA_DIR_PREFIX_ARRAY=(`for DIR in \`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datadir from $CONFIG_TABLE where content<>-1 and definedprimary='f';"\`;do $DIRNAME $DIR;done|$SORT -u|$TR '\n' ' '`)
	ERROR_CHK $? "obtain mirror segment prefixes" 2
	LOG_MSG "[INFO]:-Commencing mirror directory checks" 1
	for MIR_DIR in ${MIR_DATA_DIR_PREFIX_ARRAY[@]}
	do
		CHK_WRITE_DIR $MIR_DIR $POOL_HOST
	done
	#Build recovery array
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

POOL_HOST_DIR_VALIDATION () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PROCESS_PORT_VALUES
	$ECHO "BEGIN;" > $TMP_FILE
	ARRAY_VECTOR=0
	for FAIL_LINE in ${FAILED_INSTANCE_ARRAY[@]}
	do
		FAIL_SET_VAR $FAIL_LINE
		#Now check to see if this a primary or mirror segment
		SEG_TYPE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select definedprimary from $CONFIG_TABLE where dbid=${FAIL_QE_DBID};"`
		ERROR_CHK $? "get segment type" 2
		case $SEG_TYPE in 
			t )	#Primary segment
				NEW_HOST=$POOL_HOST
				if [ ${#PRI_POOL_HOST_IN_USE[@]} -ne 0 ];then
					DIR_CHK_LIST=${PRI_DATA_DIR_PREFIX_ARRAY[@]}
					DIR_USED_LIST=${PRI_POOL_HOST_IN_USE[@]}
					GET_FREE_DIR
				else
					NEW_DIR=$FAIL_QE_DIR
				fi
				PRI_POOL_HOST_IN_USE=(${PRI_POOL_HOST_IN_USE[@]} $NEW_DIR)
				CHK_FREE_PORT $NEW_HOST $MIN_PRI_PORT $MAX_PRI_PORT	
				NEW_PORT=$FREE_PORT
				POOL_PORT_IN_USE=(${POOL_PORT_IN_USE[@]} $NEW_PORT)
				;;
			f )	#Mirror segment
				NEW_HOST=$POOL_HOST
				if [ ${#MIR_POOL_HOST_IN_USE[@]} -ne 0 ];then
					DIR_CHK_LIST=${MIR_DATA_DIR_PREFIX_ARRAY[@]}
					DIR_USED_LIST=${MIR_POOL_HOST_IN_USE[@]}
					GET_FREE_DIR
				else
					NEW_DIR=$FAIL_QE_DIR
				fi
				MIR_POOL_HOST_IN_USE=(${MIR_POOL_HOST_IN_USE[@]} $NEW_DIR)
				CHK_FREE_PORT $NEW_HOST $MIN_MIR_PORT $MAX_MIR_PORT	
				NEW_PORT=$FREE_PORT
				POOL_PORT_IN_USE=(${POOL_PORT_IN_USE[@]} $NEW_PORT)
				;;
		esac
		$ECHO "update $CONFIG_TABLE set hostname='${NEW_HOST}', datadir='${NEW_DIR}', port=${NEW_PORT} where dbid=${FAIL_QE_DBID};" >> $TMP_FILE
		POOL_FAILED_INSTANCE_ARRAY[$ARRAY_VECTOR]="${NEW_HOST}|${NEW_PORT}|${NEW_DIR}|${FAIL_QE_DBID}|${FAIL_QE_CONTENTID}"
		((ARRAY_VECTOR=$ARRAY_VECTOR+1))
	done
	$ECHO "COMMIT;" >> $TMP_FILE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_FREE_DIR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Checking to see if $FAIL_QE_DIR in use on $POOL_HOST" 1
	QE_DIRNAME=`$DIRNAME $FAIL_QE_DIR`
	if [ `$ECHO ${DIR_USED_LIST[@]}|$GREP -c $QE_DIRNAME` -eq 1 ];then
		LOG_MSG "[INFO]:-Directory $QE_DIRNAME on pool host already allocated, looking for alternative"
		#Now cycle through the list of used directories to see which one is free to use
		for DIR in ${DIR_CHK_LIST[@]}
		do
			if [ `$ECHO ${DIR_USED_LIST[@]}|$GREP -c $DIR` -ne 0 ];then
				LOG_MSG "[INFO]:-Directory $DIR used, looking for alternative" 1
			else
				NEW_DIR=${DIR}/${SEG_PREFIX}$FAIL_QE_CONTENTID
				LOG_MSG "[INFO]:-Setting recovery directory to $NEW_DIR" 1
				break
			fi
		done
	else
		LOG_MSG "[INFO]:-Original directory not in use, will re-use $QE_DIRNAME" 1
		NEW_DIR=$FAIL_QE_DIR
	fi			
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_STATUS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	COUNT=0
	LOG_MSG "[INFO]:-Starting master database in utility mode" 1
	$MPPSTART -m
	while [ $COUNT -lt $ARRAY_INDEX ]
	do
		FAIL_SET_VAR ${FAILED_INSTANCE_ARRAY[$COUNT]}
		LOG_MSG "[INFO]:-Setting $FAIL_QE_HOST DBID=$FAIL_QE_DBID port=$FAIL_QE_PORT as active" 1
		 env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c "BEGIN; update $CONFIG_TABLE set valid = 't' where dbid = CAST($FAIL_QE_DBID AS smallint); update $CONFIG_TABLE set isprimary = definedprimary where content = CAST($FAIL_QE_CONTENTID AS smallint); COMMIT;" > /dev/null 2>&1
		RETVAL=$?
		ERROR_CHK $RETVAL "update status on Master database" 2
		((COUNT=$COUNT+1))
	done
	LOG_MSG "[INFO]:-Shutting down master database in utility mode" 1
	$MPPSTOP -m
LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap '$RM -f $TMP_FILE;ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'qfal:d:z:p:s:DB:" opt;do
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
	p ) POOL_HOST=$OPTARG ;;
	s ) POOL_DIR_SET=$OPTARG ;;
	D ) DEBUG_LEVEL=1 ;;
	B ) BATCH_DEFAULT=$OPTARG ;;
        * ) USAGE ;;
    esac
done
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
		EXIT_STATUS=1
else
	LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
CHK_DB_QD_SET
CHK_ON_PASSIVE_STANDBY
VALIDATE_PARAMS
GET_SETTING
READ_CHK
if [ $READ_COUNT -ne 0 ];then
        ERROR_EXIT "[FATAL]:-This script cannot be used on a Master read only segment instance" 2
fi
GET_MIRROR
if [ x"" != x"$POOL_HOST" ];then
	CHK_POOL_HOST
	POOL_HOST_DIR_VALIDATION
fi
if [ x"" != x"$POOL_DIR_SET" ];then
	CHK_POOL_DIR_AVAILABLE
	POOL_DIR_VALIDATION
fi
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
LOG_MSG "[INFO]:-Recovery completed successfully" 1
LOG_MSG "[INFO]:-End Main"

