#!/bin/bash
#	Filename:-		mppcreatecluster.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		MPP Conversion SE Co Internal	
#	Author:-		L Lonergan/G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Metapa Co Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright MPP database cluster
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
# 1.1	27/07/2005	G Coombe	Configured mirroring functionality
# 1.2	09/08/2005	G Coombe	Corrected CVS conversion errors.
#			Corrected error exit message display for Master init
#			failure.
# 1.3	09/08/2005	G Coombe	Corrected fault where template1 database table
#					mpp.pgdatabase has incorrect db name.
# 1.4	09/08/2005	G Coombe	Changed the create database code to use revised
#					MPP ability to now create database within Master.
# 1.5	11/09/2005	G Coombe	Updates to UPDATE_PGDATABASE in response to
#					MPP code change where dbname column on longer used.
#					Correct error message where blank lines appear in
#					the hostlist file.
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code

WORKDIR=`dirname $0`

# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
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
BACKOUT_FILE=$DEFLOGDIR/backout_mppcreatecluster_${USER_NAME}_${CUR_DATE}_$TIME
# Level of script feedback 0=small 1=verbose
VERBOSE=1
INTERACTIVE=1
MIRRORING=0
# MPP database specific parameters
MPP_USER=$USER_NAME
MPP_PASSWD=""
# System table names
MPP_TBL=mpp.mpp
MPP_HOST_TBL=mpp.host
MPP_PGDATABASE_TBL=mpp.pgdatabase
MPP_LISTENER_TBL=mpp.listener
MAX_QD_CONNECTIONS=25
unset PG_CONF_ADD_FILE
EXIT_STATUS=0
# ED_FILE search text values
PORT_TXT="#port"
LOG_STATEMENT_TXT="#log_statement ="
MAX_CONNECT_TXT="max_connections ="
LISTEN_ADR_TXT="listen_addresses"
CHKPOINT_SEG_TXT="checkpoint_segments"
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0` -c mpp_config_file [OPTIONS]"
		$ECHO 
		$ECHO "      Creates a new MPP database on a Master host and a number of"
		$ECHO "      segment instance hosts."
		$ECHO 
		$ECHO "      General options:"
		$ECHO "      -?, display this help message & exit"
		$ECHO "      -v, display version information & exit"
		$ECHO
	        $ECHO "      Configuration options:"
		$ECHO "      -c, mpp_config_file [mandatory]"
		$ECHO "          Supplies all MPP configuration information required by this utility."
		$ECHO "          Full description of all parameters contained within the example file"
		$ECHO "          supplied with this distribution."
		$ECHO "          Also see mppcreatecluster_INSTRUCTIONS file for greater detail on"
		$ECHO "          the operation and configuration of this script"
		$ECHO "      -h, mpp_hostlist_file [optional]"
		$ECHO "          Contains a list of all segment instance hostnames required to participate in"
		$ECHO "          the new MPP database. Normally set in mpp_config_file."
		$ECHO "      -p, postgresql_conf_mpp_additions [optional]"
		$ECHO "          List of additional PostgreSQL parameters to be applied to each Master/Segment"
		$ECHO "          postgresql.conf file during MPP database initialization."
		$ECHO "      -s, standby_hostname [optional]"
		$ECHO "      -o, create Master only, do not configure Segment instance hosts"
		$ECHO "      -m, maximum number of connections for master instance [default ${MAX_QD_CONNECTIONS}]"
		$ECHO
		$ECHO "      Logging options:"
		$ECHO "      -q, quiet mode, do not log progress to screen [default:- verbose output to screen]"
		$ECHO "      -a, don't ask to confirm database creation [default:- ask]"
		$ECHO "      -l, logfile_directory [optional]"
		$ECHO "	         Alternative logfile directory"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 - No problems encountered with requested operation"
		$ECHO "      1 - Warning generated, but database operational"
		$ECHO "      2 - Fatal error, database not created/started, or in an inconsistent state,"
	    	$ECHO "            see log file for failure reason."
		$ECHO
		exit $EXIT_STATUS
}

CHK_PARAMS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"

		if [ $USER_NAME == "root" ]; then
				ERROR_EXIT "[FATAL]:-Unable to run this script as $USER" 2
		fi
		if [ x"" = x"$MPPHOME" ] && [ x"" = x"$BIZHOME" ]; then
				LOG_MSG "[FATAL]:-Environment variables MPPHOME and BIZHOME not set"
				LOG_MSG "[FATAL]:-At least one of these must be set for this script to continue"
				ERROR_EXIT "[FATAL]:-Unable to continue" 2
		fi
		#Check that we can see initdb
		if [ x"$INITDB" = x"" ] || [ ! -x $INITDB ];then
			ERROR_EXIT "[FATAL]:-Unable to locate initdb" 99
		fi
		#Make sure that script has been supplied a config file
		if [ x"" = x"$CLUSTER_CONFIG" ]; then
				ERROR_EXIT "[FATAL]:-MPP configuration filename [-c] option not provided." 2
		fi

		#Check that we have a non-zero configuration file
		CHK_FILE $CLUSTER_CONFIG

		if [ $EXISTS -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Problem with $CLUSTER_CONFIG file" 2
		fi
	
		#Make sure old CLUSTER_CONFIG settings are not hanging around.
		unset PORT_BASE SEG_PREFIX DATA_DIRECTORY

		#Source the cluster configuration file
		LOG_MSG "[INFO]:-Reading MPP configuration file $CLUSTER_CONFIG"
		. $CLUSTER_CONFIG

		#Check for required definitions in the CLUSTER_CONFIG file
		if [ x"" = x"$PORT_BASE" ]; then
				ERROR_EXIT "[FATAL]:-PORT_BASE not specified in $CLUSTER_CONFIG file, is this the correct cluster configuration file." 2
		fi
		if [ x"" = x"$SEG_PREFIX" ]; then
				ERROR_EXIT "[FATAL]:-SEG_PREFIX not specified in $CLUSTER_CONFIG file, is this the correct cluster configuration file." 2
		fi
		if [ x"" = x"$DATA_DIRECTORY" ]; then
				ERROR_EXIT "[FATAL]:-DATA_DIRECTORY not specified in $CLUSTER_CONFIG file, is this the correct cluster configuration file." 2
		fi

		#Check the other files that are required to be able to continue
		FILE_LIST=($MACHINE_LIST_FILE)
		for FILE in "${FILE_LIST[@]}"
				do
				CHK_FILE $FILE
				if [ $EXISTS -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-Problem with $FILE file" 2
						LOG_MSG "[INFO]:-Completed check of file $FILE"
				fi
		done

		CHK_DUPLICATES
		#Set up the machine list array
		LOG_MSG "[INFO]:-Setting up segment instance list array"
		declare -a MACHINE_LIST=(`$CAT $MACHINE_LIST_FILE`)

		#Now process contents for the configuration file
		if [ ! "$ARRAY_NAME" ]; then
				LOG_MSG "[WARN]:-ARRAY_NAME variable not set, will provide default value"
				EXIT_STATUS=1
				ARRAY_NAME="MPP Cluster"
		fi
		export ARRAY_NAME

		if [ x"" = x"$MASTER_HOSTNAME" ]; then
				ERROR_EXIT "[FATAL]:MASTER_HOSTNAME variable not set" 2
		fi

		#Make sure that this script is running on the QD host
		if [ $MASTER_HOSTNAME != `$HOSTNAME` ]; then
				LOG_MSG "[FATAL]:-Cluster master hostname in configuration file is ${MASTER_HOSTNAME}"
				LOG_MSG "[FATAL]:-Operating system command returns $HOSTNAME"
				ERROR_EXIT "[FATAL]:-This script must be executed on the Master host $MASTER_HOSTNAME" 2
		fi


		# Deal with issue of mixed case segment names being created in all lower case on tails but mixed
		# case in head system tables.
		LWR_CASE_SEG_PREFIX=`$ECHO $SEG_PREFIX|$TR '[A-Z]' '[a-z]'`
		SEG_PREFIX=$LWR_CASE_SEG_PREFIX

		#MASTER_PORT
		if [ x"" = x"$MASTER_PORT" ]; then
				ERROR_EXIT "[FATAL]:-MASTER_PORT variable not set" 2
		fi

		#PORT_BASE
		if [ x"" = x"$PORT_BASE" ]; then
				ERROR_EXIT "[FATAL]:-PORT_BASE variable not set"2
		fi
		QE_PORT=$PORT_BASE

		#DATA_DIRECTORY
		((QE_PRIMARY_COUNT=${#DATA_DIRECTORY[@]}))
		if [ $QE_PRIMARY_COUNT -eq 0 ]; then
				ERROR_EXIT "[FATAL]:-Number of primary directories 0 (zero)" 2
		fi

		#MASTER_DIRECTORY
		if [ x"" = x"$MASTER_DIRECTORY" ]; then
				ERROR_EXIT "[FATAL]:-MASTER_DIRECTORY variable not set" 2
		fi
		#DATABASE_PREFIX
		if [ x"" = x"$DATABASE_NAME" ]; then
				LOG_MSG "[INFO]:-No DATABASE_NAME set, will exit following template1 updates"
		else
				LOG_MSG "[INFO]:-Will create database $DATABASE_NAME"
		fi
		#IP_ALLOW
		if [ ! $IP_ALLOW ]; then
				ERROR_EXIT "[FATAL]:-IP_ALLOW variable not set" 2
		fi
		#TRUSTED_SHELL
		if [ ! $TRUSTED_SHELL ]; then
				ERROR_EXIT "[FATAL]:-TRUSTED_SHELL variable not set" 2
		fi
		#CHECK_POINT_SEGMENTS
		if [ ! $CHECK_POINT_SEGMENTS ]; then
				ERROR_EXIT "[FATAL]:-CHECK_POINT_SEGMENTS variable not set" 2
		fi
		#ENCODING
		if [ ! $ENCODING ]; then
				LOG_MSG "[WARN]:-ENCODING variable not set, will set to default SQL_ASCII"
				EXIT_STATUS=1
				ENCODING="SQL_ASCII"
		fi
		#Number of QE hosts in this configuration
		((NUM_QES=${#MACHINE_LIST[*]}))
		if [ $NUM_QES -eq 0 ]; then
				ERROR_EXIT "[FATAL]:-Number of QE's 0 (zero)" 2
		fi
		#Mirror configuration
		if [ $MIRROR_PORT_BASE ]; then
				MIRRORING=1
				((NUM_MIRROR_DIRECTORY=${#MIRROR_DATA_DIRECTORY[@]}))
				if [ $NUM_MIRROR_DIRECTORY -ne $QE_PRIMARY_COUNT ]; then
						ERROR_EXIT "[FATAL]:-Number of primary directories does not match number of mirror directories" 2
				fi
		fi

		#Get IP address of the master host

		MASTER_IP_ADDRESS=(`$IFCONFIG $IFCONFIG_TXT |$GREP "inet "|$GREP -v "127.0.0"|$AWK '{print $2}'|$CUT -d: -f2`)
		if [ x"" != x"$STANDBY_HOSTNAME" ];then
			STANDBY_IP_ADDRESS=(`$TRUSTED_SHELL $STANDBY_HOSTNAME "$IFCONFIG $IFCONFIG_TXT |$GREP "inet "|$GREP -v \"127.0.0\"|$AWK '{print $2}'|$CUT -d: -f2"`)
		fi
		#Below commented out until standby QD tests completed (25/07/2005)
		##	if [ $STANDBY_IP_ALLOW ]&& [ $STANDBY_QD_MASK ]; then
		##		LOG_MSG "[INFO]:-Standby Master IP = $STANDBY_QD_IP"
		##		LOG_MSG "[INFO]:-Standby Master Netmask = $STANDBY_QD_MASK"
		##	else
		##		LOG_MSG "[INFO]:-Standby Master = off"

}

CHK_DUPLICATES () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ `$CAT $MACHINE_LIST_FILE|$GREP -v $"^$"|$WC -l` -ne `$CAT $MACHINE_LIST_FILE|$GREP -v "^$"|$SORT -u|$WC -l` ]
				then
				ERROR_EXIT "[FATAL]:-Duplicate segment instance hostname exists in $MACHINE_LIST_FILE" 2
		fi
		LOG_MSG "[INFO]:-No duplicate segment instance hostnames found, will proceed"
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_QES () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		for QE_ID in `$CAT $MACHINE_LIST_FILE|$GREP -v "${HOSTNAME}\$"|$SORT`
				do
				$PING -c 1 $QE_ID > /dev/null 2>&1
				case $? in
						0)
						LOG_MSG "[INFO]:-$QE_ID contact established";;
						1)
						ERROR_EXIT "[FATAL]:-Unable to contact $QE_ID" 2;;
						2)
						ERROR_EXIT "[FATAL]:-Unknown host $QE_ID" 2;;
				esac
				#Check to ensure have required QE directories
				for DIR in "${DATA_DIRECTORY[@]}"
						do
						LOG_MSG "[INFO]:-Checking segment instance $QE_ID directory $DIR"
						$TRUSTED_SHELL $QE_ID "$LS $DIR" >> $LOG_FILE 2>&1
						RETVAL=$?
						if [ $RETVAL -ne 0 ]; then
								ERROR_EXIT "[FATAL]:-No $DIR on segment instance $QE_ID" 2
						else
								LOG_MSG "[INFO]:-$QE_ID $DIR checked"
						fi
				done
				#Check that we have a MPPROOT directory on the QE
				$TRUSTED_SHELL $QE_ID "$LS $MPPROOT"> /dev/null 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-No $MPPROOT on segment instance $QE_ID" 2
				else
						LOG_MSG "[INFO]:-QE $QE_ID $MPPROOT checked"
				fi
		done
		#Check to ensure that instance directories do not exist on hosts
		LOG_MSG "[INFO]:-Primary segment instance directory check"
		for I in "${QE_PRIMARY_ARRAY[@]}"
				do
				SET_VAR $I
				LOG_MSG "[INFO]:-Checking $MPP_HOSTNAME for dir $MPP_DIR"
				$TRUSTED_SHELL $MPP_HOSTNAME "$LS $MPP_DIR" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -eq 0 ]; then
						ERROR_EXIT "[FATAL]:-Instance directory $MPP_DIR exists on segment instance $MPP_HOSTNAME" 2
				fi
		done
		#Check for mirror directories if nirroring configured
		if [ $MIRRORING -ne 0 ]; then
				LOG_MSG "[INFO]:-Mirror segment instance directory check"
				for I in "${QE_MIRROR_ARRAY[@]}"
						do
						SET_VAR $I
						LOG_MSG "[INFO]:-Checking $MPP_HOSTNAME for dir $MPP_DIR"
						$TRUSTED_SHELL $MPP_HOSTNAME "$LS $MPP_DIR" >> $LOG_FILE 2>&1
						RETVAL=$?
						if [ $RETVAL -eq 0 ]; then
								ERROR_EXIT "[FATAL]:-Instance directory $MPP_DIR exists on segment instance $MPP_HOSTNAME" 2
						fi
				done
		else
				LOG_MSG "[INFO]:-Mirror segment instance directory check skipped, mirroring=Off"
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_HOST_ID () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		MPP_INDEX=0
		FOUND=0
		for MPP_HOST in "${MACHINE_LIST[@]}"	
				do	
				if [ $1 == ${MACHINE_LIST[$MPP_INDEX]} ]; then
						FOUND=$MPP_INDEX
				fi
				((MPP_INDEX=$MPP_INDEX+1))
		done
		MPP_INDEX=$FOUND
		if [ `$ECHO ${MACHINE_LIST[@]}|$TR ' ' '\n'|$GREP -c "^${MASTER_HOSTNAME}$"` -eq 0 ]; then
				((MPP_INDEX=$MPP_INDEX+2))
		else
				if [ $1 == $MASTER_HOSTNAME ]; then
						MPP_INDEX=1
				else
						((MPP_INDEX=$MPP_INDEX+1))
				fi
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


CREATE_QE_ARRAY () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		. $CLUSTER_CONFIG
		if [ `$CAT ${MACHINE_LIST_FILE}|$GREP -c "${MASTER_HOSTNAME}\$"` -eq 0 ]; then
				declare -a MACHINE_LIST=(`$CAT $MACHINE_LIST_FILE`)
		else
				declare -a MACHINE_LIST=($MASTER_HOSTNAME `$CAT $MACHINE_LIST_FILE|$GREP -v "${MASTER_HOSTNAME}\$"`)
		fi
		NUM_QE_HOSTS="${#MACHINE_LIST[@]}"
		MIRROR_HOST_LIST=("${MACHINE_LIST[@]}" "${MACHINE_LIST[@]}")
		DBID_COUNT=2
		DB_COUNT=1
		LISTENERID_COUNT=2
		CONTENT_COUNT=0
		SEG_COUNT=1
		DIR_INDEX=0
		ARRAY_INDEX=0
		PORT_COUNT=1
		((QD_LISTNERID=$LISTENERID_COUNT-1))
		QDHOSTID_COUNT=1
		QE_PORT=$PORT_BASE
		((QDSEG_COUNT=$SEG_COUNT-1))
		((QDDBID_COUNT=$DBID_COUNT-1))
		((QDDB_COUNT=$DB_COUNT-1))
		((QDCONTENT_COUNT=$CONTENT_COUNT-1))
		QD_PRIMARY_ARRAY=${MASTER_HOSTNAME}:${QDHOSTID_COUNT}:${QD_LISTNERID}:${MASTER_PORT}:${MASTER_DIRECTORY}/${SEG_PREFIX}${QDSEG_COUNT}:${QDDBID_COUNT}:${DATABASE_NAME}${QDDB_COUNT}:${QDCONTENT_COUNT}
		QE_NAME_INDEX=0
		for QE_NAME in "${MACHINE_LIST[@]}"
				do
				QE_DIR_INDEX=0
				QE_MIR_VECTOR=0
				for MPP_DIR in "${DATA_DIRECTORY[@]}"
						do
						GET_HOST_ID $QE_NAME
						((MPP_PORT=$PORT_BASE+$PORT_COUNT))
						QE_PRIMARY_ARRAY[$ARRAY_INDEX]=${QE_NAME}:${MPP_INDEX}:${LISTENERID_COUNT}:${MPP_PORT}:${MPP_DIR}/${SEG_PREFIX}${SEG_COUNT}:${DBID_COUNT}:${DATABASE_PREFIX}${DB_COUNT}:$CONTENT_COUNT
						((LISTENERID_COUNT=$LISTENERID_COUNT+1))
						((DIR_INDEX=$DIR_INDEX+1))
						((MPP_DIR_INDEX=$MPP_DIR_INDEX+1))
						if [ $MIRRORING -eq 1 ]; then
								#Have mirroring configured, process mirror QE's
								LOG_MSG "[INFO]:-Configuring mirror for segment instance $QE_NAME"
								MPP_DIR=${MIRROR_DATA_DIRECTORY[$QE_MIR_VECTOR]}/${SEG_PREFIX}${SEG_COUNT}
								if [ $NUM_QE_HOSTS -le 2 ]; then
										IDX=0
										while [ $IDX -le $NUM_QE_HOSTS ]
												do
												if [ $QE_NAME ==  "${MACHINE_LIST[$IDX]}" ]; then
														((IDX=$IDX+1))
														break
												fi
												((IDX=$IDX+1))
										done
										MIR_IDX=$IDX
								else
										((MIR_IDX=$QE_NAME_INDEX+$QE_DIR_INDEX+1))
								fi
								QE_NAME="${MIRROR_HOST_LIST[$MIR_IDX]}"
								HOST_INDEX=0
								GET_HOST_ID $QE_NAME
								((HOST_INDEX=$HOST_INDEX+1))
								((MIRROR_MPP_PORT=$MIRROR_PORT_BASE+$PORT_COUNT))
								((DBID_COUNT=$DBID_COUNT+1))
								QE_MIRROR_ARRAY[$ARRAY_INDEX]=${QE_NAME}:${MPP_INDEX}:${LISTENERID_COUNT}:${MIRROR_MPP_PORT}:${MPP_DIR}:${DBID_COUNT}:${DATABASE_PREFIX}${DB_COUNT}:$CONTENT_COUNT
								((LISTENERID_COUNT=$LISTENERID_COUNT+1))
								((QE_DIR_INDEX=$QE_DIR_INDEX+1))
								((QE_MIR_VECTOR=$QE_MIR_VECTOR+1))
						fi
						if [ $NUM_QE_HOSTS -ne 1 ]; then
								((QE_DIR_INDEX=$QE_DIR_INDEX+1))
						fi
						((SEG_COUNT=$SEG_COUNT+1))
						((PORT_COUNT=$PORT_COUNT+1))
						((ARRAY_INDEX=$ARRAY_INDEX+1))
						((DBID_COUNT=$DBID_COUNT+1))
						((DB_COUNT=$DB_COUNT+1))
						((CONTENT_COUNT=$CONTENT_COUNT+1))
				done
				PORT_COUNT=1
				((QE_DIR_INDEX=$QE_DIR_INDEX+1))
				((QE_NAME_INDEX=$QE_NAME_INDEX+1))
		done
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_CONFIG () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		. $CLUSTER_CONFIG
		if [ `$CAT ${MACHINE_LIST_FILE}|$GREP -c "${MASTER_HOSTNAME}\$"` -eq 0 ]; then
				declare -a MACHINE_LIST=(`$CAT $MACHINE_LIST_FILE`)
		else
				declare -a MACHINE_LIST=($MASTER_HOSTNAME `$CAT $MACHINE_LIST_FILE|$GREP -v "${MASTER_HOSTNAME}\$"`)
		fi
		((NUM_QES=${#MACHINE_LIST[*]}))
		if [ $MIRRORING -eq 1 ]; then
				((TOTAL_SEG=${NUM_QES}*$QE_PRIMARY_COUNT))
		else
				((TOTAL_SEG=${NUM_QES}*$QE_PRIMARY_COUNT))
		fi
		if [  x"" == x"$PG_CONF_ADD_FILE" ]; then
				PG_ADD=Off
		else
				PG_ADD=On
		fi
		SET_VAR $QD_PRIMARY_ARRAY
		LOG_MSG "[INFO]:-MPP Creation Parameters"
		LOG_MSG "[INFO]:------------------------"
		LOG_MSG "[INFO]:-Master cluster name  = $ARRAY_NAME"
		LOG_MSG "[INFO]:-Master hostname      = $MPP_HOSTNAME"
		LOG_MSG "[INFO]:-MPP segment prefix   = $SEG_PREFIX"
		LOG_MSG "[INFO]:-Primary segment #    = $QE_PRIMARY_COUNT"
		LOG_MSG "[INFO]:-Master Database      = $DATABASE_NAME"
		LOG_MSG "[INFO]:-Master connections   = $MAX_QD_CONNECTIONS"
		LOG_MSG "[INFO]:-Master IP allow      = $IP_ALLOW"
		if [ x"" != x"$STANDBY_HOSTNAME" ];then
		LOG_MSG "[INFO]:-Standby Master       = $STANDBY_HOSTNAME"
		else
		LOG_MSG "[INFO]:-Standby Master       = Not Configured"
		fi
		for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
		do
			LOG_MSG "[INFO]:-Master IP address    = $MASTER_IP"
		done
		LOG_MSG "[INFO]:-Total MPP segments   = $TOTAL_SEG"
		LOG_MSG "[INFO]:-Trusted shell        = $TRUSTED_SHELL"
		LOG_MSG "[INFO]:-Checkpoint segments  = $CHECK_POINT_SEGMENTS"
		LOG_MSG "[INFO]:-Encoding             = $ENCODING"
		LOG_MSG "[INFO]:-Pg Conf add file     = $PG_ADD"
		LOG_MSG "[INFO]:-Initdb to be used    = $INITDB"
		LOG_MSG "[INFO]:-LD_LIBRARY_PATH is   = $LD_LIBRARY_PATH"
		((NUM_QES=${#MACHINE_LIST[*]}))
		LOG_MSG "[INFO]:-Number segment hosts = $NUM_QES"
		if [ $MIRROR_PORT_BASE ]; then
				LOG_MSG "[INFO]:-Mirror port base     = $MIRROR_PORT_BASE"
				((NUM_MIRROR_DIRECTORY=${#MIRROR_DATA_DIRECTORY[@]}))
				LOG_MSG "[INFO]:-Mirror segment #     = $NUM_MIRROR_DIRECTORY"
				LOG_MSG "[INFO]:-Mirroring config     = ON"
		else
				LOG_MSG "[INFO]:-Mirroring config     = OFF"
		fi
		LOG_MSG "[INFO]:-Master Primary MPP Configuration"
		LOG_MSG "[INFO]:---------------------------------"
		LOG_MSG "[INFO]:-Master port              = $MASTER_PORT"
		LOG_MSG "[INFO]:-Master instance dir      = $MPP_DIR"
		LOG_MSG "[INFO]:-QE Primary MPP Configuration"
		LOG_MSG "[INFO]:-----------------------------"
		export QD_DIR=$MPP_DIR
		for I in "${QE_PRIMARY_ARRAY[@]}"
				do
				SET_VAR $I
				LOG_MSG  "[INFO]:-Primary segment on $MPP_HOSTNAME $MPP_PORT $MPP_DIR $MPP_HOSTID $MPP_LISTENID $MPP_DBID $MPP_CONTENT"
		done
		if [ $MIRRORING -ne 0 ]; then
				LOG_MSG "[INFO]:-Segment Mirror MPP Configuration"
				LOG_MSG "[INFO]:---------------------------------"
				for I in  "${QE_MIRROR_ARRAY[@]}"
						do
						SET_VAR $I
						LOG_MSG  "[INFO]:-Mirror segment on $MPP_HOSTNAME $MPP_PORT $MPP_DIR $MPP_HOSTID $MPP_LISTENID $MPP_DBID $MPP_CONTENT"
				done
		fi
		if [ $INTERACTIVE ]; then
				$ECHO "Continue with MPP creation Yy/Nn>"
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

CREATE_QD_DB () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		SET_VAR $QD_PRIMARY_ARRAY
		CHK_DIR $MPP_DIR
		if [ $EXISTS -eq 0 ]; then
				ERROR_EXIT "[FATAL]:-Master directory $MPP_DIR already exists" 2
		fi
		LOG_MSG "[INFO]:-Setting path for plpgsql.so"
		ED_FILE ${SHARE_PATH}/$SCHEMA_FILE plpgsql.so "AS '${SHARE_PATH}/plpgsql.so'" 1
		LOG_MSG "[INFO]:-Initializing Master Postgres instance $MPP_DIR"
		RUN_COMMAND "$INITDB -E $ENCODING -D $MPP_DIR"
		BACKOUT_COMMAND "$RM -Rf $MPP_DIR"
		BACKOUT_COMMAND "$ECHO Removing Master data directory files"
		LOG_MSG "[INFO]:-Completed Master instance initialization"
		$ECHO "#MPP specific configuration parameters for Master instance database" >> ${MPP_DIR}/$PG_CONF
		$ECHO "#--------------------------------------------------------------" >> ${MPP_DIR}/$PG_CONF
		LOG_MSG "[INFO]:-Setting the Master port to $MPP_PORT"
		ED_FILE ${MPP_DIR}/$PG_CONF "$PORT_TXT" port=$MPP_PORT 0
		if [ $? -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Failed to set Master port in $PG_CONF" 2
		fi
		LOG_MSG "[INFO]:-Completed setting the Master port to $MASTER_PORT"
		LOG_MSG "[INFO]:-Setting Master logging option"
		ED_FILE ${MPP_DIR}/$PG_CONF "$LOG_STATEMENT_TXT" log_statement=all 0
		if [ $? -ne 0 ]; then
			LOG_MSG "[WARN:-Failed to set log_statement=all in ${MPP_DIR}/$PG_CONF"
			EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-Setting Master instance maximum connections to ${MAX_QD_CONNECTIONS}"
		ED_FILE ${MPP_DIR}/$PG_CONF "$MAX_CONNECT_TXT" "max_connections=${MAX_QD_CONNECTIONS}" 0
		if [ $? -ne 0 ]; then
			LOG_MSG "[WARN:-Failed to set max_connections=${MAX_QD_CONNECTIONS} in ${MPP_DIR}/$PG_CONF"
			EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-Setting Master instance check point segments"
		ED_FILE ${MPP_DIR}/$PG_CONF "$CHKPOINT_SEG_TXT" "checkpoint_segments=$CHECK_POINT_SEGMENTS" 0
		if [ $? -ne 0 ]; then
			LOG_MSG "[WARN:-Failed to set checkpoint_segments=$CHECK_POINT_SEGMENTS in ${MPP_DIR}/$PG_CONF"
			EXIT_STATUS=1
		fi
		if [  x"" != x"$PG_CONF_ADD_FILE" ]; then
				LOG_MSG "[INFO]:-Processing additional configuration parameters"
				for NEW_PARAM in `$CAT $PG_CONF_ADD_FILE|$TR -s ' '|$TR -d ' '|$GREP -v "^#"`
						do
						LOG_MSG "[INFO]:-Adding config $NEW_PARAM to Master"
						SEARCH_TXT=`$ECHO $NEW_PARAM |$CUT -d"=" -f1`
						ED_FILE ${MPP_DIR}/$PG_CONF $SEARCH_TXT $NEW_PARAM 0
				done
		fi
		LOG_MSG "[INFO]:-Setting IP Allow in $PG_HBA to value $IP_ALLOW"
		$ECHO "host    all         all         $IP_ALLOW        $QD_MASK      trust" >> ${MPP_DIR}/$PG_HBA
		if [ $? -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Failed to set IP Allow in $PG_HBA" 2
		fi
		LOG_MSG "[INFO]:-Completed setting IP Allow to $IP_ALLOW"
		PG_LOCK_COUNT=`$LS /tmp/.s.PGSQL.${MPP_PORT}.lock|$WC -l|$AWK '{print $1}'`
		if [ $PG_LOCK_COUNT -ne 0 ]; then
			ERROR_EXIT "[FATAL]-Located another postmaster process on Master host on port $MPP_PORT" 2
		fi
		LOG_MSG "[INFO]:-Starting the Master in admin mode"
		export PGPORT=$MPP_PORT;$PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o "-i -p $MPP_PORT" start
		RET_TEXT="`$PG_CTL status -D $MPP_DIR`"
		RUNNING=`$ECHO $RET_TEXT|$EGREP -c "not running|neither"`
		if [ $RUNNING -ne 0 ]; then
				$CAT ${MPP_DIR}.log|$TEE -a $LOG_FILE
				ERROR_EXIT "[FATAL]:-Failed to start the Master database in admin mode" 2
		fi
		BACKOUT_COMMAND "$RM -f /tmp/.s.PGSQL.${MPP_PORT}*"
		BACKOUT_COMMAND "$ECHO \"Removing Master lock files\""
		BACKOUT_COMMAND "$RM -f ${MPP_DIR}.log"
		BACKOUT_COMMAND "$ECHO Removing Master log file"
		BACKOUT_COMMAND "export PGPORT=$MPP_PORT;$PG_CTL -D $MPP_DIR stop"
		BACKOUT_COMMAND "$ECHO \"Stopping Master instance\""
		LOG_MSG "[INFO]:-Compleleted starting the Master in admin mode"
		$SLEEP 2
		UPDATE_MPP $MPP_DBNAME $MPP_PORT "$ARRAY_NAME" $TOTAL_SEG $MPP_DBID $MPP_CONTENT 0 X
		UPDATE_HOST $MPP_DBNAME $MPP_PORT $MPP_HOSTID $MPP_HOSTNAME
		UPDATE_LISTENER $MPP_DBNAME $MPP_PORT $MPP_LISTENID $MPP_HOSTID $MPP_PORT $MPP_DIR $MPP_USER $MPP_PASSWD
		UPDATE_PGDATABASE $MPP_DBNAME $MPP_PORT $MPP_DBID template1 $MPP_CONTENT $MPP_LISTENID t
		LOAD_QE_SYSTEM_DATA template1
		SET_VAR $QD_PRIMARY_ARRAY
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_PGDATABASE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		U_DB=$1
		U_PT=$2
		U_DBID=$3
		U_DBNAME=$4
		U_CONTENT=$5
		U_LISTENERID=$6
		U_PRIMARY=$7
		U_DB=template1
		CHK_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -A -t -c "select count(*) from $MPP_PGDATABASE_TBL where listenerid=${U_LISTENERID} and content=${U_CONTENT};" 2>/dev/null` >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Unable to obtain psql count of Master pgdatabase system table" 2
		fi
		if [ $CHK_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Adding $U_CONTENT to system pgdatabase table"
				env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -c "insert into $MPP_PGDATABASE_TBL (dbid, isprimary, content, listenerid, valid, definedprimary) values (${U_DBID},'${U_PRIMARY}',${U_CONTENT},${U_LISTENERID},'t', '${U_PRIMARY}');" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-Failed to add $U_CONTENT to system pgdatabase table" 2
				else
						LOG_MSG "[INFO]:-Successfully added $U_CONTENT to system pgdatabase table"
				fi
		else
				LOG_MSG "[INFO]:-Listenerid $U_CONTENT already exists in pgdatabase system table"
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_LISTENER () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		U_DB=$1
		U_DB=template1
		U_PT=$2
		U_LISTENERID=$3
		U_HOSTID=$4
		U_PORT=$5
		U_DIR=$6
		U_USER=$7
		U_PASSWD=$8
		CHK_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -A -t -c "select count(*) from $MPP_LISTENER_TBL where listenerid=${U_LISTENERID};" 2>/dev/null` >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Unable to obtain psql count of Master listener system table" 2
		fi
		if [ $CHK_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Adding $U_LISTENERID to system listener table"
				env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -c "insert into $MPP_LISTENER_TBL (listenerid, hostid, port, datadir, mppsuperuser, mppsuperpswd) values (${U_LISTENERID},${U_HOSTID},${U_PORT},'${U_DIR}','${U_USER}','${U_PASSWD}');" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-Failed to add $U_LISTENERID to system listener table" 2
				else
						LOG_MSG "[INFO]:-Successfully added $U_LISTENERID to system listener table"
				fi
		else
				LOG_MSG "[INFO]:-Listenerid $U_LISTENERID already exists in listener system table"
		fi

		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

LOAD_QE_SYSTEM_DATA () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		SET_VAR $QD_PRIMARY_ARRAY
		###	QD_DBNAME=$MPP_DBNAME
		TARGET_DB=$1
		QD_DBNAME=template1
		for I in "${QE_PRIMARY_ARRAY[@]}"
				do
				SET_VAR $I
				LOG_MSG "[INFO]:-Adding segment $MPP_HOSTNAME to Master system tables"
				UPDATE_HOST $QD_DBNAME $MASTER_PORT $MPP_HOSTID $MPP_HOSTNAME
				UPDATE_LISTENER $QD_DBNAME $MASTER_PORT $MPP_LISTENID $MPP_HOSTID $MPP_PORT $MPP_DIR $MPP_USER $MPP_PASSWD
				UPDATE_PGDATABASE $QD_DBNAME $MASTER_PORT $MPP_DBID template1 $MPP_CONTENT $MPP_LISTENID t
				LOG_MSG "[INFO]:-Successfully added segment $MPP_HOSTNAME to Master system tables"
		done
		if [ $MIRRORING -eq 1 ]; then
				for I in "${QE_MIRROR_ARRAY[@]}"
				do
					SET_VAR $I
					LOG_MSG "[INFO]:-Adding segment $MPP_HOSTNAME to Master system tables as mirror"
					UPDATE_HOST $QD_DBNAME $MASTER_PORT $MPP_HOSTID $MPP_HOSTNAME
					UPDATE_LISTENER $QD_DBNAME $MASTER_PORT $MPP_LISTENID $MPP_HOSTID $MPP_PORT $MPP_DIR $MPP_USER $MPP_PASSWD
					UPDATE_PGDATABASE $QD_DBNAME $MASTER_PORT $MPP_DBID template1 $MPP_CONTENT $MPP_LISTENID f
					LOG_MSG "[INFO]:-Successfully added segment $MPP_HOSTNAME to Master system tables as mirror"
				done
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_HOST () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		U_DB=$1
		U_DB=template1
		U_PT=$2
		U_HOSTID=$3
		U_HOSTNAME=$4
		#First check to ensure that U_HOSTID not already in the host table
		CHK_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -A -t -c "select count(*) from $MPP_HOST_TBL where hostid='${U_HOSTID}';" 2>/dev/null` >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Unable to obtain psql count of Master host system table" 2
		fi
		if [ $CHK_COUNT -eq 0 ]; then
				LOG_MSG "[INFO]:-Adding $U_HOSTNAME to system host table"
				env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -c "insert into $MPP_HOST_TBL (hostid, hostname) values (${U_HOSTID},'${U_HOSTNAME}');" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-Failed to add $U_HOSTNAME to system host table" 2
				else
						LOG_MSG "[INFO]:-Successfully added $U_HOSTNAME to system host table"
				fi
		else
				LOG_MSG "[INFO]:-Host $U_HOSTNAME already exists in host system table"
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

UPDATE_MPP () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		U_DB=$1
		U_DB=template1
		U_PT=$2
		U_MPPNAME="$3"
		U_NUMSEG=$4
		U_DBID=$5
		U_CONTENT=$6
		TYPE=$7
		U_HOST=$8
		if [ $TYPE -eq 0 ]; then
				env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $U_PT $U_DB -c "insert into $MPP_TBL values ('${U_MPPNAME}',${U_NUMSEG},${U_DBID},${U_CONTENT});" >> $LOG_FILE 2>&1
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
						ERROR_EXIT "[FATAL]:-Unable to update mpp system table" 2
				fi
		else
				RUN_COMMAND_REMOTE $U_HOST "env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $U_PT $U_DB -c \"insert into $MPP_TBL (mppname, numsegments, dbid, content) values ('${U_MPPNAME}',${U_NUMSEG},${U_DBID},${U_CONTENT});\" 2>&1" >> $LOG_FILE 2>&1
		fi	
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Failed to update mpp system table" 2
		fi
		LOG_MSG "[INFO]:-Successfully updated mpp system table"
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_QES_PRIMARY () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		for I in "${QE_PRIMARY_ARRAY[@]}"
		do
			SET_VAR $I
			PROCESS_QE
			LOG_MSG "[INFO]:-Processing segment $MPP_HOSTNAME"
			LOG_MSG "[INFO]:-Successfully started segment instance on $MPP_HOSTNAME"
			UPDATE_MPP $MPP_DBNAME $MPP_PORT "$ARRAY_NAME" $TOTAL_SEG $MPP_DBID $MPP_CONTENT 1 $MPP_HOSTNAME
		done
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_QES_MIRROR () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		for I in  "${QE_MIRROR_ARRAY[@]}"
		do
			SET_VAR $I
			PROCESS_QE
			LOG_MSG "[INFO]:-Processing Mirror segment $MPP_HOSTNAME"
			LOG_MSG "[INFO]:-Successfully started segment mirror instance on $MPP_HOSTNAME"
			UPDATE_MPP $MPP_DBNAME $MPP_PORT "$ARRAY_NAME" $TOTAL_SEG $MPP_DBID $MPP_CONTENT 1 $MPP_HOSTNAME
		done
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_QE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "export LD_LIBRARY_PATH=${LD_LIBRARY_PATH};$INITDB -E $ENCODING -D $MPP_DIR"
		BACKOUT_COMMAND "$TRUSTED_SHELL ${MPP_HOSTNAME} \"$RM -rf $MPP_DIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $MPP_DIR on $MPP_HOSTNAME\""
		LOG_MSG "[INFO]:-Configuring segment $PG_CONF"
		RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "$ECHO \"#MPP Specifc parameters\" >> ${MPP_DIR}/$PG_CONF"
		RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "$ECHO \"#----------------------\" >> ${MPP_DIR}/$PG_CONF"
		ED_FILE ${MPP_DIR}/$PG_CONF "$PORT_TXT" port=$MPP_PORT 0 $MPP_HOSTNAME
		ED_FILE ${MPP_DIR}/$PG_CONF "$LISTEN_ADR_TXT" listen_addresses=\'*\' 0 $MPP_HOSTNAME
		ED_FILE ${MPP_DIR}/$PG_CONF "$CHKPOINT_SEG_TXT" checkpoint_segments=$CHECK_POINT_SEGMENTS 0 $MPP_HOSTNAME
		if [ x"" != x"$PG_CONF_ADD_FILE" ]; then
			LOG_MSG "[INFO]:-Processing additional configuration parameters"
			for NEW_PARAM in `$CAT $PG_CONF_ADD_FILE|$TR -s ' '|$TR -d ' '|$GREP -v "^#"`
			do
				LOG_MSG "[INFO]:-Adding config $NEW_PARAM to segment"
				SEARCH_TXT=`$ECHO $NEW_PARAM |$CUT -d"=" -f1`
				ED_FILE ${MPP_DIR}/$PG_CONF $SEARCH_TXT $NEW_PARAM 0 $MPP_HOSTNAME
			done
		fi

		LOG_MSG "[INFO]:-End Function $FUNCNAME"
		LOG_MSG "[INFO]:-Configuring segment $PG_HBA"
		for MASTER_IP in "${MASTER_IP_ADDRESS[@]}"
		do
			RUN_COMMAND_REMOTE ${MPP_HOSTNAME} "$ECHO host	all	all	${MASTER_IP}/32	trust >> ${MPP_DIR}/$PG_HBA"
		done
		LOG_MSG "[INFO]:-Starting Functioning instance on segment ${MPP_HOSTNAME}"
		$TRUSTED_SHELL ${MPP_HOSTNAME} "export LD_LIBRARY_PATH=${LD_LIBRARY_PATH};export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" start"
		if [ $? -ne 0 ]; then
			BACKOUT_COMMAND "$ECHO \"Removing segment log file ${MPP_DIR}.log on segment $MPP_HOSTNAME\""
			BACKOUT_COMMAND "$TRUSTED_SHELL $MPP_HOSTNAME \"export LD_LIBRARY_PATH=${LD_LIBRARY_PATH};export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" -m immediate  stop\""
			$TRUSTED_SHELL ${MPP_HOSTNAME} "$CAT ${MPP_DIR}.log "|$TEE -a $LOG_FILE
			ERROR_EXIT "[FATAL]:-Failed to start segment Mirror instance database" 2
		fi	
		BACKOUT_COMMAND "$TRUSTED_SHELL ${MPP_HOSTNAME} \"$RM -f ${MPP_DIR}.log\""
		BACKOUT_COMMAND "$ECHO \"Removing segment log file ${MPP_DIR}.log on segment $MPP_HOSTNAME\""
		BACKOUT_COMMAND "$TRUSTED_SHELL $MPP_HOSTNAME \"export LD_LIBRARY_PATH=${LD_LIBRARY_PATH};export PGPORT=${MPP_PORT}; $PG_CTL -w -D $MPP_DIR -l ${MPP_DIR}.log -o \"-i -p ${MPP_PORT}\" -m immediate  stop\""
		BACKOUT_COMMAND "$ECHO \"Stopping segment instance on $MPP_HOSTNAME\""
}

STOP_QD_PRODUCTION () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		LOG_MSG "[INFO]:-Restarting the MPP cluster in production mode"
		if [ -f $WORKDIR/mppstop.sh ]; then
				$WORKDIR/mppstop.sh -a -d $QD_DIR
				RETVAL=$?
				case $RETVAL in
				0 ) LOG_MSG "[INFO]:-Successfully shutdown the new MPP array" ;;
				1 ) LOG_MSG "[WARN]:-Non fatal error from MPP array shutdown, check log files, will continue"
				    EXIT_STATUS=1 ;;
				2 ) ERROR_EXIT "[FATAL]:-Failed to stop new MPP cluster, check log file" 2
				esac
		else
				ERROR_EXIT "[FATAL]:-WORKDIR/mppstop.sh not located" 2
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_QD_PRODUCTION () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ -f $WORKDIR/mppstart.sh ]; then
				$WORKDIR/mppstart.sh -a -d $QD_DIR
				if [ $? -ne 0 ]; then
					ERROR_EXIT "[FATAL]:-Failed to start new MPP cluster, check log file" 2
				fi
		else
				ERROR_EXIT "[FATAL]:-$WORKDIR/mppstart.sh not located" 2
		fi
		LOG_MSG "[INFO]:-Completed restart of MPP cluster in production mode"
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_DATABASE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		SET_VAR $QD_PRIMARY_ARRAY
		$PSQL -p $MPP_PORT template1 -c"create database ${DATABASE_NAME};" >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:-Failed to create database $DATABASE_NAME" 2
		else
				LOG_MSG "[INFO]:-Successfully created database $DATABASE_NAME"
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
while getopts ":v'?'aqc:l:p:m:h:o:s:" opt
		do
		case $opt in
				v ) VERSION_INFO ;;
				'?' ) USAGE ;;
				a ) unset INTERACTIVE ;;
				q ) unset VERBOSE ;;
				c ) CLUSTER_CONFIG=$OPTARG ;;
				l ) LOG_DIR=$OPTARG ;;
				p ) PG_CONF_ADD_FILE=$OPTARG ;;
				h ) MACHINE_LIST_FILE=$OPTARG ;;
				m ) MAX_QD_CONNECTIONS=$OPTARG ;;
				o ) QD_ONLY=1 ;;
				s ) STANDBY_HOSTNAME=$OPTARG ;;
				* ) USAGE ;;
		esac
done

if [ x"$CLUSTER_CONFIG" = x ]; then
		USAGE
fi
if [ x"" != x"$PG_CONF_ADD_FILE" ]; then
	CHK_FILE $PG_CONF_ADD_FILE
	if [ $EXISTS -ne 0 ]; then
		ERROR_EXIT "[FATAL]:-Issue with postgres additions file $PG_CONF_ADD_FILE" 2
	fi
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
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"

CHK_PARAMS

#Now source the config file for the rest of the functions within this script
. $CLUSTER_CONFIG

CREATE_QE_ARRAY
CHK_QES
DISPLAY_CONFIG
CREATE_QD_DB		

if [ $QD_ONLY ]; then
		LOG_MSG "[INFO]:-Master only End Main"
		exit 0
fi

CREATE_QES_PRIMARY

if [ $MIRRORING -ne 0 ]; then
		CREATE_QES_MIRROR
fi

if [ x"" != x"$DATABASE_NAME" ]; then
	CREATE_DATABASE
fi

STOP_QD_PRODUCTION
if [ x"" != x"$STANDBY_HOSTNAME" ];then
	CREATE_STANDBY_QD
fi
START_QD_PRODUCTION
$ECHO "$RM $BACKOUT_FILE" >> $BACKOUT_FILE
BACKOUT_COMMAND "if [ $MASTER_HOSTNAME != \`$HOSTNAME\` ];then $ECHO \"[FATAL]:-Not on original master host $MASTER_HOSTNAME, backout script exiting!\";exit 2;fi"
LOG_MSG "[INFO]:-MPP cluster successfully created"
LOG_MSG "[INFO]:-------------------------------------------------------"
LOG_MSG "[INFO]:-To access the mpp scripts for this cluster:"
LOG_MSG "[INFO]:-In user $USER_NAME environment file add following line"
LOG_MSG "[INFO]:-export MASTER_DATA_DIRECTORY=${MASTER_DIRECTORY}/${SEG_PREFIX}0"
LOG_MSG "[INFO]:-or, use -d ${MASTER_DIRECTORY}/${SEG_PREFIX}0"
LOG_MSG "[INFO]:-option for the mpp scripts"
LOG_MSG "[INFO]:-Example ./mppstate.sh -d ${MASTER_DIRECTORY}/${SEG_PREFIX}0"
LOG_MSG "[INFO]:-Script log file = $LOG_FILE"
LOG_MSG "[INFO]:-Script backout file = $BACKOUT_FILE"
LOG_MSG "[INFO]:-To remove cluster, run"
LOG_MSG "[INFO]:-/bin/bash $BACKOUT_FILE"
LOG_MSG "[INFO]:-------------------------------------------------------"
if [ $STANDBY_QD_NAME ]; then
		$WORKDIR/mppinitstandby.bsh -h $STANDBY_QD_NAME
else
		LOG_MSG "[INFO]:-To initialize standby segment run mppinitstandby.bsh -h <standby Master hostname>"
fi

LOG_MSG "[INFO]:-End Main"

exit $EXIT_STATUS
