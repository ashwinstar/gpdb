#!/bin/bash
#	Filename:-		mpprebuildseg.sh
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
MPP_TBL=mpp.mpp
START_PROG=${WORKDIR}/mppstart.sh
STOP_PROG=${WORKDIR}/mppstop.sh
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0 `"
		$ECHO
		$ECHO "      Builds segment instances from information contained in a Master"
		$ECHO "      instance that was built via use of mpprebuildcluster.sh with -m"
		$ECHO "      option. This allows original segment instance hostnames to be "
		$ECHO "      changed following the Master instance rebuild"
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
		$ECHO "      Segment rebuild options:"
		$ECHO "      -d, <master data directory name> directory where a master database is located"
		$ECHO "         [mandatory]"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but master database operational"
		$ECHO "      2 Fatal error, database not rebuilt, or in an inconsistent state"
		$ECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

START_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Expect the master database instance to be closed down, check for postmaster.pid file
	if [ -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears Master instance operational, shutdown master instance from admin mode" 2
	else
		LOG_MSG "[INFO]:-Starting Master instance in admin mode"
		if [ -f $START_PROG ];then
			$START_PROG -qam
			ERROR_CHK $? "start Master instance in admin mode" 2
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $START_PROG" 2
		fi		
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_MASTER_DB () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ ! -f ${MASTER_DATA_DIRECTORY}/$PG_PID ];then
		ERROR_EXIT "[FATAL]:-Appears Master instance shutdown during this process, check Master instance log file" 2
	else
		LOG_MSG "[INFO]:-Commencing shutdown of Master instance from admin mode"
		if [ -f $STOP_PROG ];then
			$STOP_PROG -qam
			ERROR_CHK $? "stop Master instance from admin mode" 2
		else
			ERROR_EXIT "[FATAL]:-Unable to locate $STOP_PROG" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_SEGMENT_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MASTER_USER=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select mppsuperuser from mpp.listener where listenerid=1;"`
	ERROR_CHK $? "get master instance user name" 2
	MASTER_MPP_NAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select mppname from mpp.mpp;"`
	ERROR_CHK $? "get master instance array name" 2
	REBUILD_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select a.hostname, b.port, b.datadir, c.dbid, c.content, c.definedprimary from mpp.host a, mpp.listener b, mpp.pgdatabase c where a.hostid=b.hostid and b.listenerid=c.listenerid and c.content<>-1;"`)
	ERROR_CHK $? "get array segment instance details " 2
	MASTER_IP_ADDRESS_ALL=(`$IFCONFIG |$GREP "inet addr"|$GREP -v addr:127|$AWK '{print $2}'|$CUT -d: -f2`)	
	ERROR_CHK $? "get master host IP details" 2
	MASTER_IP_ADDRESS=(`$ECHO ${MASTER_IP_ADDRESS_ALL[@]}|$TR ' ' '\n'|$SORT -u|$TR '\n' ' '`)
	MASTER_USER=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select mppsuperuser from mpp.listener where listenerid=1;"`
	ERROR_CHK $? "get master instance super user $MASTER_USER" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	MASTER_TOTAL_SEG=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $QD_DBNAME -c "select numsegments from mpp.mpp;"`
	ERROR_CHK $? "get total instance segments" 2
	MASTER_HOST=`$HOSTNAME`
}

CHK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing parameter checks"
	STOP_BUILD=0
	if [ "$USER_NAME" != "$MASTER_USER" ] ;then
		LOG_MSG "[WARN]:-Current user account name $USER_NAME does not match mppsuperuser"
		STOP_BUILD=1
	fi
	if [ $STOP_BUILD -ne 0 ];then
		ERROR_EXIT "[FATAL]:-One or more errors encountered, please review log and correct." 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_QES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for SEG_INFO in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $SEG_INFO
		#Check hosts can be reached
		PING_HOST $SEG_HOSTNAME
		#Check that we do not already have segment directory 
		CHK_DIR $SEG_DATADIR $SEG_HOSTNAME
		if [ $EXISTS -eq 0 ];then
			ERROR_EXIT "[FATAL]:-Segment directory $SEG_DATADIR exists on $SEG_HOSTNAME" 2
		fi
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "MPP segment instance rebuild parameters"
	LOG_MSG "---------------------------------------"
	LOG_MSG "MPP Master hostname            = $MASTER_HOST"
	LOG_MSG "MPP Master data directory      = $MASTER_DATA_DIRECTORY"
	LOG_MSG "MPP Master port                = $MASTER_PORT"
	LOG_MSG "MPP Array name                 = $MASTER_MPP_NAME"
	for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
	do
		LOG_MSG "MPP Master IP addr             = $MASTER_IP"
		IP_ALLOW="`$ECHO $MASTER_IP|$CUT -d. -f1-3`.0/24"
		LOG_MSG "MPP Master IP allow            = $IP_ALLOW"
	done
	LOG_MSG "MPP Segment instance list"
	LOG_MSG "-------------------------"
	for I in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $I
		if [ "$SEG_DEFINED" == "t" ]; then
			SEG_DEFINED="Primary"
		else
			SEG_DEFINED="Mirror"
		fi
		LOG_MSG "$SEG_HOSTNAME $SEG_DATADIR $SEG_PORT  $SEG_DBID $SEG_DEFINED"
	done
	if [ $INTERACTIVE ]; then
		$ECHO  "Continue with MPP rebuild Yy/Nn>"
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


SET_VAR () {
	SEG_HOSTNAME=`$ECHO $1|$CUT -d"|" -f1`
	SEG_PORT=`$ECHO $1|$CUT -d"|" -f2`
	SEG_DATADIR=`$ECHO $1|$CUT -d"|" -f3`
	SEG_DBID=`$ECHO $1|$CUT -d"|" -f4`
	SEG_CONTENT=`$ECHO $1|$CUT -d"|" -f5`
	SEG_DEFINED=`$ECHO $1|$CUT -d"|" -f6`
}

BUILD_SEGMENT_INSTANCES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Creating segment instance databases"
	for SEG_INFO in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $SEG_INFO
		if [ `$TRUSTED_SHELL ${SEG_HOSTNAME} "$GREP -c \"${MPP_LIBRARY_PATH}/plpgsql.so\" ${SHARE_PATH}/$SCHEMA_FILE"` -eq 0 ];then
			LOG_MSG "[INFO]:-Setting path for plpgsql.so"
			ED_FILE ${SHARE_PATH}/$SCHEMA_FILE plpgsql.so "AS '${MPP_LIBRARY_PATH}/plpgsql.so'" 1 $SEG_HOSTNAME
		else
			LOG_MSG "[INFO]:-Path correct for plpgsql.so, skipping edit"
		fi	
		LOG_MSG "[INFO]:-Processing segment host $SEG_HOSTNAME port $SEG_PORT data directory $SEG_DATADIR dbid $SEG_DBID"
		RUN_COMMAND_REMOTE ${SEG_HOSTNAME} "$INITDB -E $ENCODING -D $SEG_DATADIR"
		BACKOUT_COMMAND "$TRUSTED_SHELL ${SEG_HOSTNAME} \"$RM -rf $SEG_DATADIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $SEG_DATADIR on $SEG_HOSTNAME\""
		LOG_MSG "[INFO]:-Configuring segment $PG_CONF"
		ED_FILE ${SEG_DATADIR}/$PG_CONF "#port" port=$SEG_PORT 0 $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF listen_addresses listen_addresses=\'*\' 0  $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF checkpoint_segments checkpoint_segments=$CHECK_POINT_SEGMENTS 0 $SEG_HOSTNAME
		LOG_MSG "[INFO]:-Configuring segment $PG_HBA"
		for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"		
		do
			RUN_COMMAND_REMOTE ${SEG_HOSTNAME} "$ECHO host  all     all     ${MASTER_IP}/32 trust >> ${SEG_DATADIR}/$PG_HBA"
		done
		LOG_MSG "[INFO]:-Starting Functioning instance on segment ${SEG_HOSTNAME}"
		$TRUSTED_SHELL ${SEG_HOSTNAME} "export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" start" >> $LOG_FILE 2>&1
		if [ $? -ne 0 ]; then
			BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -f ${SEG_DATADIR}.log\""
			BACKOUT_COMMAND "$ECHO \"Removing segment log file ${SEG_DATADIR}.log on segment $SEG_HOSTNAME\""
			BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -Rf ${SEG_DATADIR}\""
			BACKOUT_COMMAND "$ECHO \"Removing segment data directory $SEG_DATADIR\""
			$TRUSTED_SHELL ${SEG_HOSTNAME} "$CAT ${SEG_DATADIR}.log "|$TEE -a $LOG_FILE
			ERROR_EXIT "[FATAL]:-Failed to start segment instance database" 2
		fi
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -f ${SEG_DATADIR}.log\""
		BACKOUT_COMMAND "$ECHO \"Removing segment log file ${SEG_DATADIR}.log on segment $SEG_HOSTNAME\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"$RM -Rf ${SEG_DATADIR}\""
		BACKOUT_COMMAND "$ECHO \"Removing segment data directory $SEG_DATADIR\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $SEG_HOSTNAME \"export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" -m immediate  stop\""
		BACKOUT_COMMAND "$ECHO \"Stopping segment instance\""
		LOG_MSG "[INFO]:-Successfully started segment instance on $SEG_HOSTNAME"
		LOG_MSG "[INFO]:-Updating segment mpp.mpp system table"
		RUN_COMMAND_REMOTE $SEG_HOSTNAME "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT template1 -c \"insert into $MPP_TBL values ('${MASTER_MPP_NAME}',${MASTER_TOTAL_SEG},${SEG_DBID},${SEG_CONTENT});\"" >> $LOG_FILE 2>&1
		ERROR_CHK $? "update $SEG_HOSTNAME mpp.mpp record entry with DBID value ${SEG_DBID}" 2
		LOG_MSG "[INFO]:-Successfully updated $SEG_HOSTNAME mpp.mpp system table"
		LOG_MSG "[INFO]:-Commencing shutdown of segment instance on $SEG_HOSTNAME"
		$TRUSTED_SHELL ${SEG_HOSTNAME} "export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" stop" >> $LOG_FILE 2>&1
		ERROR_CHK $? "shutdown of instance on port $SEG_PORT data directory $SEG_DATADIR host $SEG_HOSTNAME" 2
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


UPDATE_BACKOUT_FILE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	$ECHO "$RM -f $BACKOUT_FILE" >> $BACKOUT_FILE
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

DISPLAY_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-The MPP array has now been successfully rebuilt and is now operational"
	LOG_MSG "[INFO]:-Rebuild details"
	LOG_MSG "[INFO]:-Master database host       = $MASTER_HOST"
	LOG_MSG "[INFO]:-Master data directory      = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-Master database port       = $MASTER_PORT"
	LOG_MSG "[INFO]:-To remove this array run /bin/bash logs/$BACKOUT_FILE"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
	while getopts ":vt'?'aip:qomc:s:l:d:" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			'?' ) USAGE ;;
			d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
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
if [ x"" == x"$MASTER_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply master data directory location with -d option" 2
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
START_MASTER_DB
GET_QD_DB_NAME
GET_SEGMENT_INFO
CHK_QES
CHK_PARAM
STOP_MASTER_DB
DISPLAY_PARAM
BUILD_SEGMENT_INSTANCES
UPDATE_BACKOUT_FILE
START_ARRAY
DISPLAY_INFO
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
