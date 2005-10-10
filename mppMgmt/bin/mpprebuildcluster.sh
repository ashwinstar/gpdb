#!/bin/bash
#	Filename:-		mpprebuildcluster.sh
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
QD_NAME=`hostname`
EXIT_STATUS=0
TEMP_PORT=55555
ENCODING=UNICODE
CHECK_POINT_SEGMENTS=8
MPP_TBL=mpp.mpp
BACKOUT_FILE=$DEFLOGDIR/backout_${PROG_NAME}_${USER_NAME}_${CUR_DATE}_$TIME
START_PROG=${WORKDIR}/mppstart.sh
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$TABECHO	
		$TABECHO $0 
		$TABECHO "Re-constructs an MPP array from a secured catalogue SQL file."
		$TABECHO "Will build a Master database and all segment instances detailed"
		$TABECHO "in the SQL file. Upon completion, the array will be operational"
		$TABECHO "and ready for a user data restore."
		$TABECHO
		$TABECHO "Usage:"
		$TABECHO $0 "[OPTIONS]"
		$TABECHO
		$TABECHO "General options:"
		$TABECHO "-?, display this help message & exit" 
		$TABECHO "-v, display version information & exit"
		$TABECHO
		$TABECHO "Logging options:"
		$TABECHO "-q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$TABECHO "-l, <logfile directory> alternative logfile directory [optional]"
		$TABECHO "-a, don't ask to confirm re-build [default:- ask]"
		$TABECHO "-p, set temporary database port number [default $TEMP_PORT]"
		$TABECHO
		$TABECHO "Rebuild options:"
		$TABECHO "-s, <sql source file name> [mandatory]"
		$TABECHO "-d, <temporary data directory name> directory where a temporary database can be created"
		$TABECHO "    to allow parsing of <sql source file name> [mandatory]"
		$TABECHO "-c, value of checkpoint segments [default $CHECK_POINT_SEGMENTS]"
		$TABECHO
		$TABECHO "Return codes:"
		$TABECHO "0 No problems encountered with requested operation"
		$TABECHO "1 Warning generated, but master database operational"
		$TABECHO "2 Fatal error, database not rebuilt, or in an inconsistent state"
		$TABECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

#DISPLAY_PARAM () {
#}

CREATE_TEMP_DB () {
	LOG_MSG "[INFO]:-Creating temporary parsing database"
	RUN_COMMAND "$INITDB -E $ENCODING -D ${TEMP_DATA_DIRECTORY}/parsetemp"
	LOG_MSG "[INFO]:-Successfully initialized temporary parsing database"
	LOG_MSG "[INFO]:-Setting temporary port number to $TEMP_PORT"
	ED_FILE ${TEMP_DATA_DIRECTORY}/parsetemp/$PG_CONF "#port" port=$TEMP_PORT
	if [ $? -ne 0 ]; then
		LOG_MSG "[FATAL]:-Failed to set temporary database port in ${TEMP_DATA_DIRECTORY}/parsetemp/$PG_CONF" 2
	else
		LOG_MSG "[INFO]:-Successfully set temporary database port to $TEMP_PORT"
	fi
	LOG_MSG "[INFO]:-Configuring Master $PG_HBA"
	for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
	do
		IP_ALLOW="`$ECHO $MASTER_IP|$CUT -d. -f1-3`.0/24"
		LOG_MSG "[INFO]:-Adding $IP_ALLOW to $PG_HBA file"
		$ECHO "host    all         all         $IP_ALLOW        trust" >> ${TEMP_DATA_DIRECTORY}/parsetemp/$PG_HBA
	done
	LOG_MSG "[INFO]:-Starting temporary database"
	export PGPORT=$TEMP_PORT;$PG_CTL -w -D ${TEMP_DATA_DIRECTORY}/parsetemp -l ${TEMP_DATA_DIRECTORY}/parsetemp.log -o "-i -p $TEMP_PORT" start >> $LOG_FILE 2>&1
	if [ $? -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Failed to start temporary database" 2
	else
		LOG_MSG "[INFO]:-Successfully started temporary database" 
	fi
	LOG_MSG "[INFO]:-Loading MPP metadata"
	env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $TEMP_PORT template1 -f $SQL_FILE
	if [ $? -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Failed to load $SQL_FILE into template1 database" 2
	else
		LOG_MSG "[INFO]:-Successfully loaded $SQL_FILE"
	fi
}

PARSE_SQL_FILE () {
	LOG_MSG "[INFO]:-Processing $SQL_FILE to obtain re-build parameters"
	MASTER_HOST=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select hostname from mpp.host where hostid=1;"`
	MASTER_DATA_DIRECTORY=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select a.datadir from mpp.listener a, mpp.pgdatabase b where a.listenerid=b.listenerid and b.content=-1;"`
	MASTER_PORT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select a.port from mpp.listener a, mpp.pgdatabase b where a.listenerid=b.listenerid and b.content=-1;"`
	MASTER_MPP_NAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select mppname from mpp.mpp;"`
	REBUILD_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select a.hostname, b.port, b.datadir, c.dbid, c.content, c.definedprimary from mpp.host a, mpp.listener b, mpp.pgdatabase c where a.hostid=b.hostid and b.listenerid=c.listenerid and c.content<>-1;"`)
	MASTER_IP_ADDRESS=(`$IFCONFIG |$GREP "inet addr"|$GREP -v addr:127|$AWK '{print $2}'|$CUT -d: -f2`)	
	MASTER_USER=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select mppsuperuser from mpp.listener where listenerid=1;"`
	MASTER_TOTAL_SEG=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $TEMP_PORT template1 -c "select numsegments from mpp.mpp;"`
}

CHK_PARAM () {
	LOG_MSG "[INFO]:-Commencing parameter checks"
	STOP_BUILD=0
	if [ `$HOSTNAME` != $MASTER_HOST ];then
		LOG_MSG "[WARN]:-Rebuild script not called on Master host $MASTER_HOST"
		STOP_BUILD=1
	fi
	if [ -d $MASTER_DATA_DIRECTORY ]; then
		LOG_MSG "[WARN]:-Master database directory $MASTER_DATA_DIRECTORY exists"
		STOP_BUILD=1
	fi
	if [ "$USER_NAME" != "$MASTER_USER" ] ;then
		LOG_MSG "[WARN]:-Current user account name $USER_NAME does not match mppsuperuser"
	fi
	if [ $STOP_BUILD -ne 0 ];then
		DELETE_TEMP_DB
		ERROR_EXIT "[FATAL]:-One or more errors encountered, please review log and correct." 2
	fi
}

DISPLAY_PARAM () {
	$ECHO "MPP rebuild parameters"
	$ECHO "----------------------"
	$ECHO "MPP Master hostname        = $MASTER_HOST"
	$ECHO "MPP Master data directory  = $MASTER_DATA_DIRECTORY"
	$ECHO "MPP Master port            = $MASTER_PORT"
	$ECHO "MPP array name             = $MASTER_MPP_NAME"
	$ECHO "MPP total segments         = $MASTER_TOTAL_SEG"
	for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
	do
		$ECHO "MPP Master IP addr         = $MASTER_IP"
		IP_ALLOW="`$ECHO $MASTER_IP|$CUT -d. -f1-3`.0/24"
		$ECHO "MPP Master IP allow        = $IP_ALLOW"
	done
	$ECHO "MPP Segment instance list"
	$ECHO "-------------------------"
	for I in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $I
		if [ "$SEG_DEFINED" == "t" ]; then
			SEG_DEFINED="Primary"
		else
			SEG_DEFINED="Mirror"
		fi
		$ECHO "$SEG_HOSTNAME $SEG_DATADIR $SEG_PORT $SEG_DEFINED"
	done
	if [ $INTERACTIVE ]; then
		$ECHO -n "Continue with MPP rebuild Yy/Nn>"
		read REPLY
		if [ -z $REPLY ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
			EXIT_STATUS=1
			DELETE_TEMP_DB
			exit $EXIT_STATUS
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
			LOG_MSG "[WARN]:-User abort requested, Script Exits!"
			EXIT_STATUS=1
			DELETE_TEMP_DB
			exit $EXIT_STATUS
		fi
	fi
}

BUILD_MPP_MASTER () {
	LOG_MSG "[INFO]:-Commencong rebuild of MPP array"
	LOG_MSG "[INFO]:-Creating Master database"
	RUN_COMMAND "$INITDB -E $ENCODING -D $MASTER_DATA_DIRECTORY"
	BACKOUT_COMMAND "$RM -Rf $MASTER_DATA_DIRECTORY"
	BACKOUT_COMMAND "$ECHO \"Removing Master instance directory\""
	LOG_MSG "[INFO]:-Setting the Master port to $MASTER_PORT"
	ED_FILE ${MASTER_DATA_DIRECTORY}/$PG_CONF "#port" port=$MASTER_PORT
	if [ $? -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Failed to set Master port in $PG_CONF" 2
	fi
	LOG_MSG "[INFO]:-Configuring Master $PG_HBA"
	for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
	do
		IP_ALLOW="`$ECHO $MASTER_IP|$CUT -d. -f1-3`.0/24"
		LOG_MSG "[INFO]:-Adding $IP_ALLOW to $PG_HBA file"
		$ECHO "host    all         all         $IP_ALLOW        trust" >> ${MASTER_DATA_DIRECTORY}/$PG_HBA
	done
	LOG_MSG "[INFO]:-Starting the Master database"
	export PGPORT=$MASTER_PORT;$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT" start >> $LOG_FILE 2>&1
	if [ $? -ne 0 ]; then
		$CAT ${MASTER_DATA_DIRECTORY}.log|$TEE -a $LOG_FILE
		ERROR_EXIT "[FATAL]:-Failed to start the Master database" 2
	fi
	BACKOUT_COMMAND "$KILL -9 \`$PS ax|$GREP postmaster|$GREP $MASTER_DATA_DIRECTORY |$GREP ${MASTER_PORT}|$AWK '{print \$1}'\`"
	BACKOUT_COMMAND "$ECHO Removing Master process"
	BACKOUT_COMMAND "$RM -f /tmp/.s.PGSQL.${MASTER_PORT}*"
	BACKOUT_COMMAND "$ECHO Removing Master lock files"
	LOG_MSG "[INFO]:-Loading Master MPP metadata"
	env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT template1 -f $SQL_FILE
	if [ $? -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Failed to load $SQL_FILE into template1 database on Master database" 2
	else
		LOG_MSG "[INFO]:-Successfully loaded $SQL_FILE on Master"
	fi
	LOG_MSG "[INFO]:-Deleting temporary database"
	DELETE_TEMP_DB
	LOG_MSG "[INFO]:-Commencing shutdown of Master database"
	export PGPORT=$MASTER_PORT;$PG_CTL -w -D $MASTER_DATA_DIRECTORY -l ${MASTER_DATA_DIRECTORY}.log -o "-i -p $MASTER_PORT" stop >> $LOG_FILE 2>&1
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
	LOG_MSG "[INFO]:-Creating segment instance databases"
	for SEG_INFO in "${REBUILD_ARRAY[@]}"
	do
		SET_VAR $SEG_INFO
		LOG_MSG "[INFO]:-Processing segment host $SEG_HOSTNAME port $SEG_PORT data directory $SEG_DATADIR"
		RUN_COMMAND_REMOTE ${SEG_HOSTNAME} "$INITDB -E $ENCODING -D $SEG_DATADIR"
		BACKOUT_COMMAND "$TRUSTED_SHELL ${SEG_HOSTNAME} \"$RM -rf $SEG_DATADIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $SEG_DATADIR on $SEG_HOSTNAME\""
		LOG_MSG "[INFO]:-Configuring segment $PG_CONF"
		ED_FILE ${SEG_DATADIR}/$PG_CONF "#port" port=$SEG_PORT $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF listen_addresses listen_addresses=\'*\' $SEG_HOSTNAME
		ED_FILE ${SEG_DATADIR}/$PG_CONF checkpoint_segments checkpoint_segments=$CHECK_POINT_SEGMENTS $SEG_HOSTNAME
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
		RUN_COMMAND_REMOTE $SEG_HOSTNAME "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT template1 -c \"insert into $MPP_TBL values ('${MASTER_MPP_NAME}',${MASTER_TOTAL_SEG},${SEG_DBID},${SEG_CONTENT});\" >> $LOG_FILE 2>&1"
		LOG_MSG "[INFO]:-Successfully updated $SEG_HOSTNAME mpp.mpp system table"
		LOG_MSG "[INFO]:-Commencing shutdown of segment instance on $SEG_HOSTNAME"
		$TRUSTED_SHELL ${SEG_HOSTNAME} "export PGPORT=${SEG_PORT}; $PG_CTL -w -D $SEG_DATADIR -l ${SEG_DATADIR}.log -o \"-i -p ${SEG_PORT}\" stop" >> $LOG_FILE 2>&1
	done	
}

DELETE_TEMP_DB () {
	LOG_MSG "[INFO]:-Stopping temporary database"
	export PGPORT=$TEMP_PORT;$PG_CTL -w -D ${TEMP_DATA_DIRECTORY}/parsetemp -l ${TEMP_DATA_DIRECTORY}/parsetemp.log -o "-i -p $TEMP_PORT" stop >> $LOG_FILE 2>&1
	if [ $? -ne 0 ]; then
		LOG_MSG "[WARN]:-Issue with temporary database shutdown, may need to manually kill PID"
		ERROR_EXIT=1
	else
		LOG_MSG "[INFO]:-Successfully shutdown temporary database"
	fi
	LOG_MSG "[INFO]:-Removing temporary database directory"
	$RM -Rf ${TEMP_DATA_DIRECTORY}/parsetemp
	LOG_MSG "[INFO]:-Deleteing temporary database log file"
	$RM -Rf ${TEMP_DATA_DIRECTORY}/parsetemp.log
}

UPDATE_BACKOUT_FILE () {
	$ECHO "$RM -f $BACKOUT_FILE" >> $BACKOUT_FILE
}

START_ARRAY () {
	CHK_FILE $START_PROG
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Could not locate $START_PROG to start MPP array" 2
	else
		LOG_MSG "[INFO]:-Starting the MPP array"
		$START_PROG -d $MASTER_DATA_DIRECTORY -a
	fi
}

DISPLAY_INFO () {
	LOG_MSG "[INFO]:-The MPP array has now been successfully rebuilt and is now operational"
	LOG_MSG "[INFO]:-Rebuild details"
	LOG_MSG "[INFO]:-Master database host       = $MASTER_HOST"
	LOG_MSG "[INFO]:-Master data directory      = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-Master database port       = $MASTER_PORT"
	LOG_MSG "[INFO]:-To remove this array run /bin/bash logs/$BACKOUT_FILE"
}

#******************************************************************************
# Main Section
#******************************************************************************
if [ $# -ne 0 ]; then
	while getopts ":vt'?'ap:qc:s:l:d:" opt;do
		case $opt in
			v ) VERSION_INFO ;;
			t ) DISPLAY_PARAM ;;
			'?' ) USAGE ;;
			d ) TEMP_DATA_DIRECTORY=$OPTARG ;;
			a ) unset INTERACTIVE ;;
			q ) unset VERBOSE ;;
			l ) LOG_DIR=$OPTARG ;;
			s ) SQL_FILE=$OPTARG ;;
			c ) CHECK_POINT_SEGMENTS=$OPTARG ;;
			p ) TEMP_PORT=$OPTARG ;;
			* ) USAGE ;;
		esac
	done	
fi

LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" = x"$SQL_FILE" ] || [ x"" = x"$TEMP_DATA_DIRECTORY" ]; then
	ERROR_EXIT "[FATAL]:-Must supply file name of sql file and temporary database directory" 2
fi
CHK_FILE $SQL_FILE
if [ $EXISTS -ne 0 ]; then
	ERROR_EXIT "[FATAL]:-Issue with sql source file $SQL_FILE, check file and retry" 2
fi
CHK_DIR $TEMP_DATA_DIRECTORY
if [ $EXISTS -ne 0 ]; then
	ERROR_EXIT "[FATAL]:-Issue with temporary database directory $TEMP_DATA_DIRECTORY" 2
fi
CREATE_TEMP_DB
PARSE_SQL_FILE
CHK_PARAM
DISPLAY_PARAM
BUILD_MPP_MASTER
BUILD_SEGMENT_INSTANCES
UPDATE_BACKOUT_FILE
START_ARRAY
DISPLAY_INFO
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
