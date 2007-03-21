#!/bin/bash
#	Filename:-		gpcreateseg.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Dec 2006
#	Release stat:-		Released
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright MPP database cluster
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code

WORKDIR=`dirname $0`

# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
FUNCTIONS=$WORKDIR/gp_bash_fuctions.sh
if [ -f $FUNCTIONS ]; then
		. $FUNCTIONS
else
		echo "[FATAL]:-Cannot source $FUNCTIONS file Script Exits!"
		exit 2
fi
#******************************************************************************
# Script Specific Variables
#******************************************************************************
# Log file that will record script actions
CUR_DATE=`$DATE +%Y%m%d`
TIME=`$DATE +%H%M%S`
PROG_NAME=`$BASENAME $0`
# Level of script feedback 0=small 1=verbose
unset VERBOSE
# MPP database specific parameters
MPP_USER=$USER_NAME
# System table names
MPP_TBL=gp_id
MPP_CONFIGURATION_TBL=gp_configuration
unset PG_CONF_ADD_FILE
EXIT_STATUS=0
# ED_FILE search text values
PORT_TXT="#port"
LOG_STATEMENT_TXT="#log_statement ="
LISTEN_ADR_TXT="listen_addresses"
CHKPOINT_SEG_TXT="checkpoint_segments"
LOG_STATS_TXT="stats_command_string="

TMP_PG_HBA=/tmp/pg_hba_conf_master.$$
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpcreatecluster.sh, this should not"
		$ECHO "      be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

SET_VAR () {
                I=$1
                MPP_HOSTNAME=`$ECHO $I|$CUT -d: -f1`
                MPP_HOSTID=`$ECHO $I|$CUT -d: -f2`
                MPP_LISTENID=`$ECHO $I|$CUT -d: -f3`
                MPP_PORT=`$ECHO $I|$CUT -d: -f4`
                MPP_DIR=`$ECHO $I|$CUT -d: -f5`
                MPP_DBID=`$ECHO $I|$CUT -d: -f6`
                MPP_DBNAME=`$ECHO $I|$CUT -d: -f7`
                MPP_CONTENT=`$ECHO $I|$CUT -d: -f8`
}

PARA_EXIT () {
	if [ $1 -ne 0 ];then
		$ECHO "FAILED:$SEGMENT_LINE" >> $PARALLEL_STATUS_FILE
		LOG_MSG "[FATAL][$INST_COUNT]:-Failed $2"
		exit 2
	else
		LOG_MSG "[INFO][$INST_COUNT]:-Completed $2"
	fi
}

PROCESS_QE () {
		LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
		LOG_MSG "[INFO][$INST_COUNT]:-Processing segment $MPP_HOSTNAME"
		$TRUSTED_SHELL ${MPP_HOSTNAME} "$EXPORT_LIB_PATH;$INITDB -E $ENCODING -D $MPP_DIR --locale=$LOCALE_SETTING --max_connections=$QE_MAX_CONNECT --shared_buffers=$QE_SHARED_BUFFERS" >> $LOG_FILE 2>&1
		RETVAL=$?
		BACKOUT_COMMAND "$TRUSTED_SHELL ${MPP_HOSTNAME} \"$RM -rf $MPP_DIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $MPP_DIR on $MPP_HOSTNAME\""
		PARA_EXIT $RETVAL "to start segment instance database $MPP_HOSTNAME $MPP_DIR"
		LOG_MSG "[INFO][$INST_COUNT]:-Configuring segment $PG_CONF"
		$TRUSTED_SHELL ${MPP_HOSTNAME} "$ECHO \"#MPP Specifc parameters\" >> ${MPP_DIR}/$PG_CONF"
		RETVAL=$?
		PARA_EXIT $RETVAL "Update ${MPP_DIR}/$PG_CONF file"
		$TRUSTED_SHELL ${MPP_HOSTNAME} "$ECHO \"#----------------------\" >> ${MPP_DIR}/$PG_CONF"
		RETVAL=$?
		PARA_EXIT $RETVAL "Update ${MPP_DIR}/$PG_CONF file"
		ED_FILE ${MPP_DIR}/$PG_CONF "$PORT_TXT" port=$MPP_PORT 0 $MPP_HOSTNAME
		PARA_EXIT $RETVAL "Update port number to $MPP_PORT"
		ED_FILE ${MPP_DIR}/$PG_CONF "$LISTEN_ADR_TXT" listen_addresses=\'*\' 0 $MPP_HOSTNAME
		PARA_EXIT $RETVAL "Update listen address"
		ED_FILE ${MPP_DIR}/$PG_CONF "$CHKPOINT_SEG_TXT" checkpoint_segments=$CHECK_POINT_SEGMENTS 0 $MPP_HOSTNAME
		PARA_EXIT $RETVAL "Update checkpoint segments"
		if [ x"" != x"$PG_CONF_ADD_FILE" ]; then
			LOG_MSG "[INFO][$INST_COUNT]:-Processing additional configuration parameters"
			for NEW_PARAM in `$CAT $PG_CONF_ADD_FILE|$TR -s ' '|$TR -d ' '|$GREP -v "^#"`
			do
				LOG_MSG "[INFO][$INST_COUNT]:-Adding config $NEW_PARAM to segment"
				SEARCH_TXT=`$ECHO $NEW_PARAM |$CUT -d"=" -f1`
				ED_FILE ${MPP_DIR}/$PG_CONF $SEARCH_TXT $NEW_PARAM 0 $MPP_HOSTNAME
				PARA_EXIT $RETVAL "Update $PG_CONF $SEARCH_TXT $NEW_PARAM"
			done
		fi
		LOG_MSG "[INFO][$INST_COUNT]:-Configuring segment $PG_HBA"
		for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
		do
			$TRUSTED_SHELL ${MPP_HOSTNAME} "$ECHO host	all	all	${MASTER_IP}/32	trust >> ${MPP_DIR}/$PG_HBA"
			PARA_EXIT $? "Update $PG_HBA for master IP address ${MASTER_IP}"
		done
		if [ x"" != x"$STANDBY_HOSTNAME" ];then
			LOG_MSG "[INFO][$INST_COUNT]:-Processing Standby master IP address for segment instances"
			for STANDBY_IP in "${STANDBY_IP_ADDRESS[@]}"
			do
				$TRUSTED_SHELL ${MPP_HOSTNAME} "$ECHO host	all	all	${STANDBY_IP}/32	trust >> ${MPP_DIR}/$PG_HBA"
				PARA_EXIT $? "Update $PG_HBA for master standby address ${STANDBY_IP}"
			done
		fi
		for QE_IP in "${QE_IP_ARRAY[@]}"
		do
				$TRUSTED_SHELL ${MPP_HOSTNAME} "$ECHO host  all     all     ${QE_IP}/32 trust >> ${MPP_DIR}/$PG_HBA"
				PARA_EXIT $? "Update $PG_HBA for segment IP address ${QE_IP}"
		done
		LOG_MSG "[INFO][$INST_COUNT]:-Starting Functioning instance on segment ${MPP_HOSTNAME}"
		$TRUSTED_SHELL ${MPP_HOSTNAME} "$EXPORT_LIB_PATH;export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" start" >> $LOG_FILE 2>&1
		if [ $? -ne 0 ]; then
			BACKOUT_COMMAND "$ECHO \"Removing segment log file ${MPP_DIR}.log on segment $MPP_HOSTNAME\""
			BACKOUT_COMMAND "$TRUSTED_SHELL $MPP_HOSTNAME \"${EXPORT_LIB_PATH};export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" -m immediate  stop\""
			$TRUSTED_SHELL ${MPP_HOSTNAME} "$CAT ${MPP_DIR}.log "|$TEE -a $LOG_FILE
			$ECHO "FAILED:$SEGMENT_LINE" >> $PARALLEL_STATUS_FILE
			LOG_MSG "[FATAL][$INST_COUNT]:-to start segment Mirror instance database"
			exit 2
		fi	
		BACKOUT_COMMAND "$TRUSTED_SHELL ${MPP_HOSTNAME} \"$RM -f ${MPP_DIR}.log\""
		BACKOUT_COMMAND "$ECHO \"Removing segment log file ${MPP_DIR}.log on segment $MPP_HOSTNAME\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $MPP_HOSTNAME \"${EXPORT_LIB_PATH};export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" -m immediate  stop\""
		BACKOUT_COMMAND "$ECHO \"Stopping segment instance on $MPP_HOSTNAME\""
		LOG_MSG "[INFO][$INST_COUNT]:-Successfully started segment instance on $MPP_HOSTNAME"
		UPDATE_MPP $MPP_DBNAME $MPP_PORT "$ARRAY_NAME" $TOTAL_SEG $MPP_DBID $MPP_CONTENT 1 $MPP_HOSTNAME
		LOG_MSG "[INFO]:-[$INST_COUNT]-End Function $FUNCNAME"
}

UPDATE_MPP () {
                LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
                U_DB=$1
                U_DB=$DEFAULTDB
                U_PT=$2
                U_MPPNAME="$3"
                U_NUMSEG=$4
                U_DBID=$5
                U_CONTENT=$6
                TYPE=$7
                U_HOST=$8
                if [ $TYPE -eq 0 ]; then
                                $EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $U_PT $U_DB -c "insert into $MPP_TBL values ('${U_MPPNAME}',${U_NUMSEG},${U_DBID},${U_CONTENT});" >> $LOG_FILE 2>&1
                                PARA_EXIT $? "add $U_DBID to Master gp_id"
                else
                                $TRUSTED_SHELL $U_HOST "$EXPORT_LIB_PATH;env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $U_PT $U_DB -c \"insert into $MPP_TBL (gpname, numsegments, dbid, content) values ('${U_MPPNAME}',${U_NUMSEG},${U_DBID},${U_CONTENT});\" " >> $LOG_FILE 2>&1
				PARA_EXIT $? "add $U_DBID to Master gp_id"
                fi
                LOG_MSG "[INFO][$INST_COUNT]:-Successfully updated mpp system table"
                LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED:$SEGMENT_LINE" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'aiqe:c:l:p:m:h:on:s:" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		q ) unset VERBOSE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
PARENT_PID=$1;shift		#PID of calling gpcreatecluster.sh program
CHK_CALL
TYPE=$1;shift
case $TYPE in
	1 )
		SEGMENT_LINE=$1;shift		#String used to build segment instance
		SET_VAR $SEGMENT_LINE
		INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
		BACKOUT_FILE=/tmp/gpsegcreate.sh_backout.$$
		LOG_FILE=$1;shift		#Central logging file
		LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
		LOG_MSG "[INFO][$INST_COUNT]:-Command line options passed to utility = $*"
		TMP_MASTER_IP_ADDRESS=$1;shift	#List of IP addresses for the master instance
		MASTER_IP_ADDRESS=(`$ECHO $TMP_MASTER_IP_ADDRESS|$TR '~' ' '`)
		TMP_QE_IP_ARRAY=$1;shift	#List of segment IP addresses
		QE_IP_ARRAY=(`$ECHO $TMP_QE_IP_ARRAY|$TR '~' ' '`)	
		TMP_STANDBY_IP_ADDRESS=$1;shift #List of IP addresses for standby master
		PROCESS_QE
		$ECHO "COMPLETED:$SEGMENT_LINE" >> $PARALLEL_STATUS_FILE
		;;
esac
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
