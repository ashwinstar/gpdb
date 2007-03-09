#!/bin/bash
#	Filename:-		mppinitstandby.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released.
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Creates a MPP master instance warm standby.
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
TIME=`$DATE +%H%M%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
START_MPPSYNC=1
UPDATE_CATALOG=1
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
	if [ -f ${MPPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
		$MORE ${MPPDOCDIR}/$HELP_DOC_NAME
		exit 0
	else
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Initalizes a standby Master host instance. Script will shutdown the"
		$ECHO "      Master instance to be able to complete this operation"
		$ECHO "      Will create a copy of the Master data directory on the designated"
		$ECHO "      standby host and start the database replication process"
		$ECHO
		$ECHO "      Usage:"
		$ECHO "      $0 [OPTIONS]"
		$ECHO
		$ECHO "      General options:"
		$ECHO "      -?, display this help message & exit" 
		$ECHO "      -v, display version information & exit"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
		$ECHO "      -a, don't ask to confirm re-build [default:- ask]"
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO
		$ECHO "      Standby initialization options:"
		$ECHO "      -s, <standby hostname> [mandatory]" 
		$ECHO "      -i, do not initialize mppsynchmaster process on standby master host [default:- initialize]"
		$ECHO "      -n, do not update system catalog tables [default:- add standby host details]"
		$ECHO "          Only use this option if standby host already recorded in MPP system catalog"
		$ECHO "          tables and only need to re-sync the standby master data directory"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but master database active and standby sync process active"
		$ECHO "      2 Fatal error, standby sync process failed, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

CHECK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check that we are not trying to create a standby instance on this host
	if [ "$MASTER_STANDBY_HOSTNAME" == "`$HOSTNAME`" ];then
		ERROR_EXIT "[FATAL]:-Cannot run this script on the standby master host" 2
	fi
	#Check to see that hostname exists and can be reached
	PING_HOST $MASTER_STANDBY_HOSTNAME
	#Check to see if we already have a standby master configured
	if [ $UPDATE_CATALOG -eq 1 ];then
	    # there is a function to do this in mpp_bash_functions.sh
		STANDBY_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where content=-1 and dbid<>1 ;"`
		ERROR_CHK $? "obtained standby master host count" 2
		if [ $STANDBY_COUNT -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Standby master host already configured" 2
		fi
	fi
	#Check to ensure that MASTER_DATA_DIRECTORY is not a file on the standby host
	FILE_THERE=`$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "if [ -f $MASTER_DATA_DIRECTORY ];then echo 1;else echo 0;fi"`
	if [ $FILE_THERE -eq 1 ];then
		ERROR_EXIT "[FATAL]:-Directory $MASTER_DATA_DIRECTORY is a file on $MASTER_STANDBY_HOSTNAME" 2
	fi
	#Now check to see if have a MASTER_DATA_DIRECTORY on new host
	CHK_DIR $MASTER_DATA_DIRECTORY $MASTER_STANDBY_HOSTNAME
	if [ $EXISTS -eq 0 ];then
		LOG_MSG "[WARN]:-Located directory $MASTER_DATA_DIRECTORY on $MASTER_STANDBY_HOSTNAME" 1
		#Now see if we have a pid file
		GET_PG_PID_ACTIVE $MASTER_PORT $MASTER_STANDBY_HOSTNAME
		if [ $PID -ne 0 ] && [ x"" != x"$FAIL_PRI_SEG" ];then
			LOG_MSG "[WARN]:-Active read only master detected on $MASTER_STANDBY_HOSTNAME" 1
			LOG_MSG "[INFO]:-Calling close down routine" 1
			$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$MPPSTOP -d $MASTER_DATA_DIRECTORY -m"
		fi
	else
		#Do not have a directory, but check to ensure we do not have a sync process
		GET_PG_PID_ACTIVE $MASTER_PORT $MASTER_STANDBY_HOSTNAME
		if [ $PID -ne 0 ];then
			LOG_MSG "[FATAL]:-Have a warm master standby process PID=$PID on $MASTER_STANDBY_HOSTNAME" 1
			LOG_MSG "[FATAL]:-But no $MASTER_DATA_DIRECTORY directory on $MASTER_STANDBY_HOSTNAME" 1
			LOG_MSG "[INFO]:-Manually remove this process on $MASTER_STANDBY_HOSTNAME and re-run this utility" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		fi
		if [ $PG_LOCK_TMP -ne 0 ] || [ x"" != x"$PG_LOCK_NETSTAT" ];then
		#Either have a lock file or a netstat entry
			LOG_MSG "[FATAL]:-No $MASTER_DATA_DIRECTORY on $MASTER_STANDBY_HOSTNAME but, have a possible" 1
			LOG_MSG "[FATAL]:-Warm master standby process on $MASTER_STANDBY_HOSTNAME" 1
			LOG_MSG "[INFO]:-Manually remove this process on $MASTER_STANDBY_HOSTNAME and re-run this utility" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		fi
	fi
	#Now check to ensure that hostname supplied matches unix hostname output
	SSH_HOSTNAME=`$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME $HOSTNAME|$TR -d ' '`
	ERROR_CHK $? "obtain remote hostname value" 2
	if [ $SSH_HOSTNAME != $MASTER_STANDBY_HOSTNAME ];then
		LOG_MSG "[WARN]:-Hostname supplied does not match hostname output on $MASTER_STANDBY_HOSTNAME" 1
		LOG_MSG "[INFO]:-Setting standby hostname to $SSH_HOSTNAME" 1
		MASTER_STANDBY_HOSTNAME=$SSH_HOSTNAME
	fi
	LOG_MSG "[INFO]:-Obtaining a list of segment instances" 1
	GET_QE_DETAILS
	LOG_MSG "[INFO]:-Testing remote access to $MASTER_STANDBY_HOSTNAME" 1
	MASTER_STANDBY_TOP_LEVEL_DIR=`$DIRNAME $MASTER_STANDBY_DATA_DIRECTORY`
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$ECHO test > ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test1.$$"	
	$ECHO test > /tmp/remote_test2.$$
	RUN_COMMAND "$SCP -pr /tmp/remote_test2.$$ ${MASTER_STANDBY_HOSTNAME}:${MASTER_STANDBY_TOP_LEVEL_DIR}"
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -f ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test1.$$"
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -f ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test2.$$"
	RUN_COMMAND "$RM -f /tmp/remote_test2.$$"	
	STANDBY_IP_ADDRESS_ALL=(`$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "$IFCONFIG $IFCONFIG_TXT |$GREP \"inet \"|$GREP -v \"127.0.0\"|$AWK '{print \\$2}'|$CUT -d: -f2" 2>>$LOG_FILE`)
	ERROR_CHK $? "obtain IP address of standby Master host" 2
	LOG_MSG "[INFO]:-Standby address array = ${STANDBY_IP_ADDRESS_ALL[@]}" 1
	STANDBY_IP_ADDRESS=(`$ECHO ${STANDBY_IP_ADDRESS_ALL[@]}|$TR ' ' '\n'|$SORT -u|$TR '\n' ' '`)
	MASTER_MPP_NAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select mppname from $MPP_MPP_TABLE;"`
	ERROR_CHK $? "obtain master array name" 2
	#Check to see if there are any other MPP host on the proposed standby host
	IN_USE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${MASTER_STANDBY_HOSTNAME}' ;"`
	ERROR_CHK $? "obtain standby host in use count" 2
	if [ $IN_USE_COUNT -ne 0 ];then
		LOG_MSG "[WARN]:-There are other segment instances active on $MASTER_STANDBY_HOSTNAME" 1
		LOG_MSG "[WARN]:-This may have an impact on overall MPP instance performance" 1
		if [ $UPDATE_CATALOG -eq 1 ];then
		#Check to ensure that port requested not already in use on that host
			PORT_IN_USE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where hostname='${MASTER_STANDBY_HOSTNAME}' and port=${MASTER_STANDBY_PORT};"`
			ERROR_CHK $? "obtain standby port check" 2
			if [ $PORT_IN_USE_COUNT -ne 0 ];then
				ERROR_EXIT "[FATAL]:-Port $MASTER_STANDBY_PORT already assigned on ${MASTER_STANDBY_HOSTNAME}" 2
			fi
		fi
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_PG_HBA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for TNODE in "${QE_ARRAY[@]}"
	do
		MPP_HOSTNAME=`$ECHO $TNODE|$CUT -d'|' -f1`
		MPP_DIR=`$ECHO $TNODE|$CUT -d'|' -f2`
		for STANDBY_IP in "${STANDBY_IP_ADDRESS[@]}"
		do
			CHK_COUNT=`$TRUSTED_SHELL $MPP_HOSTNAME "$GREP -c \"${STANDBY_IP}/32\" ${MPP_DIR}/$PG_HBA"`
			if [ $CHK_COUNT -eq 0 ];then
			LOG_MSG "[INFO]:-Adding address of $MASTER_STANDBY_HOSTNAME to $MPP_HOSTNAME $PG_HBA file" 1
			RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "$ECHO host  all     all     ${STANDBY_IP}/32        trust >> ${MPP_DIR}/$PG_HBA"
			else
			LOG_MSG "[INFO]:-Standby IP address already exists in $MPP_HOSTNAME ${MPP_DIR}/$PG_HBA" 1
			fi
		done
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#obtain a list of database names that need to be updated
	STANDBY_CATALOG_UPDATE $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_PORT
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears Master instance shutdown during this process, check Master instance log file" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of Master instance" 1
		if [ -f $MPPSTOP ];then
			$MPPSTOP -d $MASTER_DATA_DIRECTORY -af
			ERROR_CHK $? "stop Master instance" 1
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $MPPSTOP" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP standby master initialization parameters" 1
	LOG_MSG "--------------------------------------------" 1
	LOG_MSG "[INFO]:-MPP Master hostname               = $MASTER_HOSTNAME" 1
	LOG_MSG "[INFO]:-MPP Master data directory         = $MASTER_DATA_DIRECTORY" 1
	LOG_MSG "[INFO]:-MPP Master port                   = $MASTER_PORT" 1
	LOG_MSG "[INFO]:-MPP Master standby hostname       = $MASTER_STANDBY_HOSTNAME" 1
	LOG_MSG "[INFO]:-MPP Master standby port           = $MASTER_STANDBY_PORT" 1
	LOG_MSG "[INFO]:-MPP Array name                    = $MASTER_MPP_NAME" 1
	LOG_MSG "[INFO]:-MPP Master standby data directory = $MASTER_STANDBY_DATA_DIRECTORY" 1
	if [ x"" == x"$START_MPPSYNC" ];then
		LOG_MSG "[INFO]:-MPP Start mppsynmaster process    = Off" 1
	else
		LOG_MSG "[INFO]:-MPP Start mppsynmaster process    = On" 1
	fi
	if [ $UPDATE_CATALOG -eq 1 ];then
		LOG_MSG "[INFO]:-MPP update system catalog         = On" 1
	else
		LOG_MSG "[INFO]:-MPP update system catalog         = Off" 1
	fi
	if [ x"" != x"$FAIL_PRI_SEG" ];then
		LOG_MSG "[INFO]:-MPP standby build type            = Dual active Read Only" 1
	fi
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP Master standby initialization Yy/Nn>"
		read REPLY
		if [ -z $REPLY ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STANDBY_ACTIVATE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check if we have a mppsyncmaster process running
	GET_PG_PID_ACTIVE $MASTER_PORT $MASTER_STANDBY_HOSTNAME
	if [ $PID -ne 0 ];then
		LOG_MSG "[INFO]:-Stopping sync process on $MASTER_STANDBY_HOSTNAME" 1
		STOP_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY
	fi
	#Check that the proposed master standby directory does not already exist
	CHK_DIR $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_HOSTNAME
	if [ $EXISTS -eq 0 ];then
		LOG_MSG "[WARN]:-Directory $MASTER_STANDBY_DATA_DIRECTORY exists on $MASTER_STANDBY_HOSTNAME"  1
		LOG_MSG "[INFO]:-Deleting the $MASTER_STANDBY_DATA_DIRECTORY on $MASTER_STANDBY_HOSTNAME" 1
		RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -Rf $MASTER_STANDBY_DATA_DIRECTORY"
		EXIT_STATUS=1
	fi
	LOG_MSG "[INFO]:-Starting remote copy of $MASTER_DATA_DIRECTORY to Master standby host" 1
	C_DIR=`pwd`;cd ${MASTER_DATA_DIRECTORY};sync;cd $C_DIR
	$TRUSTED_COPY -pr $MASTER_DATA_DIRECTORY ${MASTER_STANDBY_HOSTNAME}:${MASTER_STANDBY_DATA_DIRECTORY} 2>>$LOG_FILE
	ERROR_CHK $? "remote copy of $MASTER_DATA_DIRECTORY to $MASTER_STANDBY_HOSTNAME" 2
	if [ x"" != x"$START_MPPSYNC" ];then
		START_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_PORT
	else
		LOG_MSG "[INFO]:-Process mppsyncmaster not started on $MASTER_STANDBY_HOSTNAME as -i supplied" 1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


START_ARRAY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CHK_FILE $MPPSTART
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $MPPSTART to start MPP instance" 2
	else
		LOG_MSG "[INFO]:-Starting the MPP instance" 1
		$MPPSTART -d $MASTER_DATA_DIRECTORY -a
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

FAIL_PRIMARY_SEGMENTS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CHK_FILE $MPPSTART $MASTER_STANDBY_HOSTNAME
        if [ $EXISTS -eq 1 ];then
                ERROR_EXIT "[FATAL]:-File $MPPSTART does not exist on $MASTER_STANDBY_HOSTNAME" 2
        fi
	CHK_FILE $MPPSTOP $MASTER_STANDBY_HOSTNAME
        if [ $EXISTS -eq 1 ];then
                ERROR_EXIT "[FATAL]:-File $MPPSTOP does not exist on $MASTER_STANDBY_HOSTNAME" 2
        fi
	#Start the readonly master segment in utility mode
	LOG_MSG "[INFO]:-Starting read only master segment instance in utility mode" 1
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$MPPSTART -d $MASTER_DATA_DIRECTORY -m"
	ERROR_CHK $? "start read only master segment instance" 2
	#Now set primary segment instances port numbers to a negative number
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MASTER_PORT $DEFAULTDB -c\" update $CONFIG_TABLE set port=dbid*-1 where definedprimary='t' and content<>-1;\""
	ERROR_CHK $? "update primary segment port numbers" 2
	#Now delete any reference to a master standby instance
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MASTER_PORT $DEFAULTDB -c\" delete from $CONFIG_TABLE where content=-1 and definedprimary='f';\""
	ERROR_CHK $? "delete master standby record" 2
	#Now change the master segment instance hostname
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MASTER_PORT $DEFAULTDB -c\" update $CONFIG_TABLE set hostname='$MASTER_STANDBY_HOSTNAME' where content=-1;\""
	ERROR_CHK $? "update master instance hostname" 2
	#Now shutdown the read only master from utility mode
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$MPPSTOP -d $MASTER_DATA_DIRECTORY -m"
	#Now start up the read only master
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "export BIZHOME=$BIZHOME;export MPPHOME=$MPPHOME;$PG_CTL -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log start"
	ERROR_CHK $? "start readonly master instance" 2
	#Now force an update of mpp_pgdatabase
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "$PSQL -p $MASTER_PORT $DEFAULTDB -c\" \l\"" >/dev/null 2>&1
	$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "$PSQL -p $MASTER_PORT $DEFAULTDB -c\" \l\"" >/dev/null 2>&1
	LOG_MSG "[INFO]:-Forced update of mpp_pgdatabase complete" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":v'?'ainfqs:l:!D" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			! ) SCRIPT_USAGE=1
			    USAGE ;;
			s ) MASTER_STANDBY_HOSTNAME=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			i ) unset START_MPPSYNC ;;
			n ) UPDATE_CATALOG=0 ;;
			f ) FAIL_PRI_SEG=1 ;;
			D ) DEBUG_LEVEL=1 ;;
			* ) USAGE ;;
		esac
	done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
		EXIT_STATUS=1
		BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
		BACKOUT_FILE=$LOG_DIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
	fi
else
	BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
fi
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ $FAIL_PRI_SEG ] && [ $UPDATE_CATALOG -eq 1 ];then
	LOG_MSG "[INFO]:-Option -f supplied setting update catalog to off" 1
	UPDATE_CATALOG=0
fi
if [ $FAIL_PRI_SEG ] && [ $START_MPPSYNC ];then
	LOG_MSG "[INFO]:-Option -f supplied setting start mppsync process to off"
	unset START_MPPSYNC
fi
CHK_ON_PASSIVE_STANDBY
MASTER_HOSTNAME=`$HOSTNAME`
CHK_DB_RUNNING
CHK_MIRRORS_CONFIGURED
if [ $FAIL_PRI_SEG ] && [ $MIRROR_COUNT -eq 1 ];then
	ERROR_EXIT "[FATAL]:-Cannot use -f option if mirroring is not configured" 2
fi
GET_QD_DB_NAME
if [ x"" == x"$MASTER_STANDBY_HOSTNAME" ];then
	ERROR_EXIT "[FATAL]:-Must supply master standby hostname location with -s option" 2
fi
MASTER_STANDBY_DATA_DIRECTORY=$MASTER_DATA_DIRECTORY
MASTER_STANDBY_PORT=$MASTER_PORT
LOG_MSG "[INFO]:-Start Main"
CHECK_PARAM
if [ x"" != x"$INTERACTIVE" ];then 
	DISPLAY_PARAM
fi
if [ $UPDATE_CATALOG -eq 1 ];then
	UPDATE_CATALOG
else
	LOG_MSG "[WARN]:-By passing catalog update as -n option passed to script" 1
	EXIT_STATUS=1
fi
UPDATE_PG_HBA
STOP_MASTER_DB
STANDBY_ACTIVATE
START_ARRAY
if [ $FAIL_PRI_SEG ];then
	FAIL_PRIMARY_SEGMENTS
	LOG_MSG "[INFO]:------------------------------------------------" 1
	LOG_MSG "[INFO]:-Successfully created a read only master standby" 1
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
