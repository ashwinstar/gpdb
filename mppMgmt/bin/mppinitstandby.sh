#!/bin/bash
#	Filename:-		mppinitstandby.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Not yet complete, 1st phase only.
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
TIME=`$DATE +%H%M%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
unset START_MPPSYNC
MPP_TBL=mpp_mpp
START_PROG=${WORKDIR}/mppstart.sh
STOP_PROG=${WORKDIR}/mppstop.sh
UPDATE_CATALOG=1
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		GET_QD_DB_NAME
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Initalizes a standby Master host instance. Script will shutdown the"
		$ECHO "      Master array to be able to complete this operation"
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
		$ECHO
		$ECHO "      Standby initialization options:"
		$ECHO "      -s, <standby hostname> [mandatory]" 
		$ECHO "      -i, initialize mppsynchmaster process on standby master host [default:- do not initialize]"
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
		STANDBY_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from mpp_configuration where content=-1 and dbid<>1 ;"`
		ERROR_CHK $? "obtained standby master host count" 2
		if [ $STANDBY_COUNT -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Standby master host already configured" 2
		fi
	fi
	LOG_MSG "[INFO]:-Obtaining a list of segment instances"
	GET_QE_DETAILS
	LOG_MSG "[INFO]:-Testing remote access to $MASTER_STANDBY_HOSTNAME"
	MASTER_STANDBY_TOP_LEVEL_DIR=`$DIRNAME $MASTER_STANDBY_DATA_DIRECTORY`
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$ECHO test ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test1.$$"	
	$ECHO test > /tmp/remote_test2.$$
	RUN_COMMAND "$SCP -pr /tmp/remote_test2.$$ ${MASTER_STANDBY_HOSTNAME}:${MASTER_STANDBY_TOP_LEVEL_DIR}"
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -f ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test1.$$"
	RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -f ${MASTER_STANDBY_TOP_LEVEL_DIR}/remote_test2.$$"
	RUN_COMMAND "$RM -f /tmp/remote_test2.$$"	
	STANDBY_IP_ADDRESS_ALL=(`$TRUSTED_SHELL $MASTER_STANDBY_HOSTNAME "$IFCONFIG $IFCONFIG_TXT |$GREP \"inet \"|$GREP -v \"127.0.0\"|$AWK '{print \\$2}'|$CUT -d: -f2" 2>>$LOG_FILE`)
	STANDBY_IP_ADDRESS=(`$ECHO ${STANDBY_IP_ADDRESS_ALL[@]}|$TR ' ' '\n'|$SORT -u|$TR '\n' ' '`)
	ERROR_CHK $? "obtain IP address of standby Master host" 2
	MASTER_MPP_NAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select mppname from mpp_mpp;"`
	ERROR_CHK $? "obtain master array name" 2
	#Check to see if there are any other MPP host on the proposed standby host
	IN_USE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from mpp_configuration where hostname='${MASTER_STANDBY_HOSTNAME}' ;"`
	ERROR_CHK $? "obtain standby host in use count" 2
	if [ $IN_USE_COUNT -ne 0 ];then
		LOG_MSG "[WARN]:-There are other segment instances active on $MASTER_STANDBY_HOSTNAME, this may have an impact on overall MPP cluster performance"
		if [ $UPDATE_CATALOG -eq 1 ];then
		#Check to ensure that port requested not already in use on that host
			PORT_IN_USE_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from mpp_configuration where hostname='${MASTER_STANDBY_HOSTNAME}' and port=${MASTER_STANDBY_PORT};"`
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
		LOG_MSG "[INFO]:-Adding address of $MASTER_STANDBY_HOSTNAME to $MPP_HOSTNAME $PG_HBA file"
		for STANDBY_IP in "${STANDBY_IP_ADDRESS[@]}"
		do
			RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "$ECHO host  all     all     ${STANDBY_IP}/32        trust >> ${MPP_DIR}/$PG_HBA"
		done
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#obtain a list of database names that need to be updated
	GET_DB_LIST
	for DB in "${DB_ARRAY[@]}"
	do
		STANDBY_CATALOG_UPDATE $MASTER_STANDBY_HOSTNAME $DB $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_PORT
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears Master instance shutdown during this process, check Master instance log file" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of Master instance"
		if [ -f $STOP_PROG ];then
			$STOP_PROG -qa
			ERROR_CHK $? "stop Master instance" 2
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $STOP_PROG" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "MPP standby QD initialization parameters"
	LOG_MSG "----------------------------------------"
	LOG_MSG "MPP Master hostname               = $MASTER_HOSTNAME"
	LOG_MSG "MPP Master data directory         = $MASTER_DATA_DIRECTORY"
	LOG_MSG "MPP Master port                   = $MASTER_PORT"
	LOG_MSG "MPP Master standby hostname       = $MASTER_STANDBY_HOSTNAME"
	LOG_MSG "MPP Master standby port           = $MASTER_STANDBY_PORT"
	LOG_MSG "MPP Array name                    = $MASTER_MPP_NAME"
	LOG_MSG "MPP Master standby data directory = $MASTER_STANDBY_DATA_DIRECTORY"
	if [ x"" == x"$START_MPPSYNC" ];then
		LOG_MSG "MPP Start mppsynmaster process    = Off"
	else
		LOG_MSG "MPP Start mppsynmaster process    = On"
	fi
	if [ $UPDATE_CATALOG -eq 1 ];then
		LOG_MSG "MPP update system catalog         = On"
	else
		LOG_MSG "MPP update system catalog         = Off"
	fi
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP Master standby initialization Yy/Nn>"
		read REPLY
		if [ -z $REPLY ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
			EXIT_STATUS=1
			exit $EXIT_STATUS
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
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
		LOG_MSG "[INFO]:-Stopping sync process on $MASTER_STANDBY_HOSTNAME"
		STOP_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY
	fi
	#Check that the proposed master standby directory does not already exist
	CHK_DIR $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_HOSTNAME
	if [ $EXISTS -eq 0 ];then
		LOG_MSG "[WARN]:-Directory $MASTER_STANDBY_DATA_DIRECTORY exists on $MASTER_STANDBY_HOSTNAME" 
		LOG_MSG "[INFO]:-Deleting the $MASTER_STANDBY_DATA_DIRECTORY on $MASTER_STANDBY_HOSTNAME"
		RUN_COMMAND_REMOTE $MASTER_STANDBY_HOSTNAME "$RM -Rf $MASTER_STANDBY_DATA_DIRECTORY"
		EXIT_STATUS=1
	fi
	LOG_MSG "[INFO]:-Issuing sync on disk sub-system $MASTER_DATA_DIRECTORY"
	C_DIR=`pwd`;cd ${MASTER_DATA_DIRECTORY};sync;cd $C_DIR
	$TRUSTED_COPY -pr $MASTER_DATA_DIRECTORY ${MASTER_STANDBY_HOSTNAME}:${MASTER_STANDBY_DATA_DIRECTORY} 2>>$LOG_FILE
	ERROR_CHK $? "remote copy of $MASTER_DATA_DIRECTORY to $MASTER_STANDBY_HOSTNAME" 2
	if [ x"" != x"$START_MPPSYNC" ];then
		START_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY $MASTER_STANDBY_PORT
	else
		LOG_MSG "[INFO]:-Process mppsyncmaster not started on $MASTER_STANDBY_HOSTNAME as -i not supplied"
	fi
	LOG_MSG "[INFO]:-Copying maintenance scripts to standby QD $MASTER_STANDBY_HOSTNAME"
	TAR_NAME=Scripts_tar_initstandby_copy_${CUR_DATE}.tar.z
	LOG_MSG "[INFO]:-Scripts can be found in $TAR_NAME file on $MASTER_STANDBY_HOSTNAME"
	C_DIR=`pwd`
	cd $SCRIPTS_DIR
	$TAR cvzf $TAR_NAME ./* >> $LOG_FILE 2>&1
	$TRUSTED_COPY -pr $SCRIPTS_DIR/$TAR_NAME ${MASTER_STANDBY_HOSTNAME}:$SCRIPTS_DIR 2>> $LOG_FILE
	ERROR_CHK $? "copy scripts to $MASTER_STANDBY_HOSTNAME" 1
	$RM -f $TAR_NAME
	cd $C_DIR
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


START_ARRAY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CHK_FILE $START_PROG
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $START_PROG to start MPP array" 2
	else
		LOG_MSG "[INFO]:-Starting the MPP array"
		$START_PROG -d $MASTER_DATA_DIRECTORY -a
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":v'?'ainqs:l:" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			s ) MASTER_STANDBY_HOSTNAME=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			i ) START_MPPSYNC=1 ;;
			n ) UPDATE_CATALOG=0 ;;
			* ) USAGE ;;
		esac
	done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
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
CHK_ON_PASSIVE_STANDBY
MASTER_HOSTNAME=`$HOSTNAME`
CHK_DB_RUNNING
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
	UPDATE_PG_HBA
else
	LOG_MSG "[WARN]:-By passing catalog update as -n option passed to script"
	EXIT_STATUS=1
fi
STOP_MASTER_DB
STANDBY_ACTIVATE
START_ARRAY
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
