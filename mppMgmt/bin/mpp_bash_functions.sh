#!/bin/bash
#	Filename:-		mpp_bash_functions.sh
#	Version:-		1.0
#	Updated:-		20/07/2005
#	Status:-		MPP Conversion SE Co Internal	
#	Author:-		G L Coombe (Greenplum)
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Common functions used by various scripts
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#			cdbheadsync.bsh utility.
#******************************************************************************
# Detailed Description
#******************************************************************************
#
#******************************************************************************
# Prep Code
#
#***************************************************************
# Location Functions
#******************************************************************************

#CMDPATH is the list of locations to search for commands, in precedence order
declare -a CMDPATH
CMDPATH=(/usr/kerberos/bin /usr/sfw/bin /opt/sfw/bin /usr/local/bin /usr/ucb /bin /usr/bin /sbin /usr/sbin)

#MPPPATH is the list of possible locations for the Bizgres binaries, in precedence order
declare -a MPPPATH
MPPPATH=( $MPPHOME $BIZHOME )

#MPP_UNIQUE_COMMAND is used to identify the binary directory
MPP_UNIQUE_COMMAND=cdb_dump

findCmdInPath() {
		cmdtofind=$1

		for pathel in ${CMDPATH[@]}
				do
				CMD=$pathel/$cmdtofind
				if [ -x $CMD ]; then
						echo $CMD
						return
				fi
		done

		return "Problem in mppbash_functions, command $cmdtofind not found in COMMAND path. You will need to edit the script named mppbash_functions.sh to properly locate the needed commands for your platform."
}

findMppPath() {
		cmdtofind=$MPP_UNIQUE_COMMAND

		for pathel in ${MPPPATH[@]}
				do
				CMD=`find $pathel -name $cmdtofind | tail -1`
				if [ -x $CMD ]; then
						echo $CMD
						return
				fi
		done
		return "Problem in mppbash_functions, command $cmdtofind not found in MPP path. Try setting BIZHOME to the location of your MPP distribution"

}

#******************************************************************************
# OS Command Variables
#******************************************************************************
AWK=`findCmdInPath awk`
BASENAME=`findCmdInPath basename`
CAT=`findCmdInPath cat`
CLEAR=`findCmdInPath clear`
CKSUM=`findCmdInPath cksum`
CUT=`findCmdInPath cut`
DATE=`findCmdInPath date`
DD=`findCmdInPath dd`
DIRNAME=`findCmdInPath dirname`
DF=`findCmdInPath df`
DU=`findCmdInPath du`
ECHO=`findCmdInPath echo`
#ETHTOOL=`findCmdInPath ethtool`
EXPR=`findCmdInPath expr`
TABECHO=$ECHO
PROMPT="$ECHO -n"
#TABECHO="$ECHO -e \t"
#PROMPT="$ECHO -n -e \t"
GREP=`findCmdInPath grep`
HEAD=`findCmdInPath head`
HOSTNAME=`findCmdInPath hostname`
IPCS=`findCmdInPath ipcs`
KILL=`findCmdInPath kill`
LS=`findCmdInPath ls`
MV=`findCmdInPath mv`
MKDIR=`findCmdInPath mkdir`
MKFIFO=`findCmdInPath mkfifo`
NETSTAT=`findCmdInPath netstat`
PING=`findCmdInPath ping`
PS=`findCmdInPath ps`
RCP=`findCmdInPath rcp`
RSH=`findCmdInPath rsh`
RM=`findCmdInPath rm`
SCP=`findCmdInPath scp`
SLEEP=`findCmdInPath sleep`
SORT=`findCmdInPath sort`
SPLIT=`findCmdInPath split`
SSH=`findCmdInPath ssh`
TAIL=`findCmdInPath tail`
TEE=`findCmdInPath tee`
TOUCH=`findCmdInPath touch`
#PTIME=`findCmdInPath ptime`
TR=`findCmdInPath tr`
WC=`findCmdInPath wc`
WHICH=`findCmdInPath which`
WHOAMI=`findCmdInPath whoami`

#***************#******************************************************************************
# Script Specific Variables
#******************************************************************************
# By default set error logging level to verbose
VERBOSE=1
USER_NAME=`$WHOAMI`
PROG_NAME=`echo $0 | $TR -d '-'`
PROG_NAME=`$BASENAME $PROG_NAME`
CALL_HOST=`$HOSTNAME|$CUT -d. -f1`

#******************************************************************************
# Locate the postgres routines from the MPP release
#******************************************************************************
PSQLBIN=`findMppPath`
PSQLBIN=`$DIRNAME $PSQLBIN`


#******************************************************************************
# MPP Command Variables
#******************************************************************************
INITDB=$PSQLBIN/initdb
CDBLOADER=$CMDBIN/loader.sh
PG_CTL=$PSQLBIN/pg_ctl
PG_DUMP=$PSQLBIN/pg_dump
PG_RESTORE=$PSQLBIN/pg_restore
PSQL=$PSQLBIN/psql


#******************************************************************************
# General Variables
#******************************************************************************
PG_PID=postmaster.pid
PG_OPT=postmaster.opts
PG_CONF=postgresql.conf
PG_HBA=pg_hba.conf
TRUSTED_SHELL="$SSH"
TRUSTED_COPY="$SCP"
PG_CONF_ADD_FILE=$WORKDIR/postgresql_conf_cdb_additions
RECOVER_FLAG=/tmp/active_recovery_in_progress

#******************************************************************************
# Functions
#******************************************************************************
CHK_DB_QD_SET () {
		if [ ! $QD_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-QD_DATA_DIRECTORY parameter not set, update user start-up file [i.e. .bashrc]." 2
		fi
}

VERSION_INFO () {
		VERSION=`$HEAD -10 $0|$GREP -i Version: |$AWK '{print $3}'`
		STATUS=`$HEAD -10 $0|$GREP -i Status: | $AWK '{print $3" "$4" "$5}'`
		UPDATE=`$HEAD -10 $0|$GREP -i Updated:|$AWK '{print $3}'`
		CHK_SUM=`$CKSUM $0|$AWK '{print $1" "$2}'`
		$ECHO "`basename $0` Version   = $VERSION"
		$ECHO "`basename $0` Status    = $STATUS"
		$ECHO "`basename $0` Update    = $UPDATE"
		$ECHO "`basename $0` Check sum = $CHK_SUM"
		exit 0
}

LOG_MSG () {
		TIME=`$DATE +%H":"%M":"%S`
		CUR_DATE=`$DATE +%Y%m%d`
		if [ $VERBOSE ]; then
				$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1" | $TEE -a $LOG_FILE
		else
				$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1" >> $LOG_FILE
		fi
}


ERROR_EXIT () {
		TIME=`$DATE +%H":"%M":"%S`
		CUR_DATE=`$DATE +%Y%m%d`
		$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1 Script Exiting!" | $TEE -a $LOG_FILE
		if [ $BACKOUT_FILE ]; then
				if [ -s $BACKOUT_FILE ]; then
						LOG_MSG "[WARN]:-Script has left MPP configuration in an incomplete state"
						LOG_MSG "[WARN]:-Run command /bin/bash $BACKOUT_FILE to remove these changes"
						$ECHO "$RM -f $BACKOUT_FILE" >> $BACKOUT_FILE
				fi
		fi
		exit $2
}

CHK_EXISTS_TABLE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		TABLENAME=$1
		QD_PORT=$2
		QD_DBNAME=$3
		$PSQL -q -p  $QD_PORT $QD_DBNAME -c"\d $TABLENAME;" -o /dev/null
		EXISTS=$?
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_FILE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		FILENAME=$1
		LOG_MSG "[INFO]:-Checking file $FILENAME"
		if [ ! -s $FILENAME ] || [ ! -r $FILENAME ]
				then
				EXISTS=1
		else
				EXISTS=0
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
CHK_DIR () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		DIR_NAME=$1
		LOG_MSG "[INFO]:-Checking directory $DIR_NAME"
		if [ ! -d $DIR_NAME ]
				then
				EXISTS=1
		else
				EXISTS=0
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QD_PORT () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		QD_DATA_DIRECTORY=$1
		if [ ! -d $QD_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-No $QD_DATA_DIRECTORY directory" 2
		fi
		QD_PORT=`$GREP "^port" $QD_DATA_DIRECTORY/$PG_CONF |$GREP -v "^#"|$CUT -d= -f2|$AWK '{print $1}'`
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_DB_RUNNING () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ ! -d $QD_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-No QD $QD_DATA_DIRECTORY directory" 2
		fi
		if [ ! -f $QD_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:-No $QD_DATA_DIRECTORY/$PG_PID file" 2
		fi		
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QD_DB_NAME () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_PORT $QD_DATA_DIRECTORY

		QD_DBNAME=`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -p $QD_PORT template1 -A -t -c"\l" 2>/dev/null|$GREP -v template|$HEAD -1|$CUT -d"|" -f1` > /dev/null 2>&1

		if [ ! $QD_DBNAME ]; then
				LOG_MSG "[INFO]:-Unable to obtain QD database name, setting value to template1"
				QD_DBNAME=template1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
GET_QE_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_ARRAY=(`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port from cdb.listener a, cdb.host b where listenerid<> (select listenerid from cdb.pgdatabase where content=-1) and a.hostid=b.hostid;" 2>/dev/null`) > /dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain QE host details from QD db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_PRIMARY_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME

		QE_PRIMARY_ARRAY=(`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port from cdb.listener a, cdb.host b where listenerid<> (select listenerid from cdb.pgdatabase where content=-1) and a.hostid=b.hostid and listenerid in (select listenerid from cdb.pgdatabase where isprimary='t') order by a.datadir;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain QE primary host details from QD db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_MIRROR_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME

		QE_MIRROR_ARRAY=(`env PGOPTIONS="-c cdb_session_role=utility" $PSQL -q -p $QD_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port from cdb.listener a, cdb.host b where listenerid<> (select listenerid from cdb.pgdatabase where content=-1) and a.hostid=b.hostid and listenerid in (select listenerid from cdb.pgdatabase where isprimary='f') order by a.datadir;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain QE mirror host details from QD db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_HOSTNAMES () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		QD_DATA_DIRECTORY=$1
		GET_QD_PORT $QD_DATA_DIRECTORY
		CHK_DB_RUNNING
		GET_QD_DB_NAME
		unset HOSTNAME_LIST
		HOSTNAME_LIST=`$PSQL -p $QD_PORT $QD_DBNAME -A -t -c"select distinct(hostname) from cdb.host;"` > /dev/null 2>&1
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

INVALID_SEG_CHECK () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		echo "This facility not supported by MPP as at 20/07/2005"
		exit
		GET_QD_DB_NAME
		INVALID_COUNT=`$PSQL -p $QD_PORT $QD_DBNAME -A -t -c"select count(*) from cdb.pgdatabase where valid <> 't';"`
		return $INVALID_COUNT
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PING_HOSTS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_HOSTNAMES
		unset PING_OK
		unset PING_FAIL
		unset PING_UNKNOWN
		O=0;F=0;U=0
		for PING_NAME in $HOSTNAME_LIST
				do
				$PING -c 1 $PING_NAME > /dev/null 2>&1
				case $? in
						0) PING_OK[$O]=$PING_NAME
						let O=$O+1;;
						1) PING_FAIL[$F]=$PING_NAME
						let F=$F+1;;
						2) PING_UNKNOWN[$U]=$PING_NAME
						let U=$U+1;;
				esac
		done
		if [ ${#PING_FAIL[@]} -gt 0 ] || [ ${#PING_UNKNOWN[@]} -gt 0 ]; then
				ERROR=1
		else ERROR=0
		fi
		return $ERROR
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_REMOTE_PID_ACTIVE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		# Expects hostname and port number
		HOST=$1
		PID=$2
		REMOTE_PID=`$TRUSTED_SHELL $HOST "$CAT /tmp/.s.PGSQL.${PID}.lock|$HEAD -1 2>/dev/null"`
		if [ -z $REMOTE_PID ]; then
				REMOTE_PID=0
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_POST_PIDS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_HOSTNAMES
		unset POST_OK
		unset POST_FAIL
		O=0;F=0
		for POST_NAME in $HOSTNAME_LIST
				do
				POST_COUNT=`$TRUSTED_SHELL -n $POST_NAME "$PS ax|$GREP postmaster|$GREP -v grep|$WC -l"|$AWK '{print $1}'`
				case $POST_COUNT in
						0) POST_FAIL[$O]=$POST_NAME
						let O=$O+1;;
						*) POST_OK[$F]=$POST_NAME
						let F=$F+1;;
				esac
		done
		if [ ${#POST_FAIL[@]} -gt 0 ]; then
				ERROR=1
		else
				ERROR=0
		fi

		return $ERROR
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RUN_COMMAND () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COMMAND="$1"
		LOG_MSG "[INFO]:-Commencing local $COMMAND"
		eval $COMMAND  >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				ERROR_EXIT "[FATAL]:- Command $COMMAND failed with error status $RETVAL" 2
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RUN_COMMAND_REMOTE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		HOST=$1
		COMMAND=$2
		LOG_MSG "[INFO]:-Commencing remote $TRUSTED_SHELL $HOST $COMMAND"
		$TRUSTED_SHELL $HOST $COMMAND  >> $LOG_FILE 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then ERROR_EXIT "[FATAL]:- Command $COMMAND on $HOST failed with error status $RETVAL" 2
		else LOG_MSG "[INFO]:-Completed $TRUSTED_SHELL $HOST $COMMAND"
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

BACKOUT_COMMAND () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COMMAND=$1
		if [ ! -f $BACKOUT_FILE ]; then
				$ECHO $COMMAND > $BACKOUT_FILE
		else
				$CAT $BACKOUT_FILE > /tmp/backout_file.tmp.$$
				$ECHO $COMMAND > $BACKOUT_FILE
				$CAT /tmp/backout_file.tmp.$$ >> $BACKOUT_FILE
				$RM -f /tmp/backout_file.tmp.$$
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_PRIMARY_ARRAY () {
		#MPP Checked 25/07/2005
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		QE_LIST=($1)
		PORT_BASE=$2
		INSTANCES_PER_QE=$3
		DATA_DIRECTORY=($4)
		INST_PREFIX=$5
		SEG_PREFIX=$6
		ARRAY_INDEX=0
		INST_COUNT=1
		DIRECTORY_VECTOR=0
		((PRI_QD_PORT=$PORT_BASE))
		PORT_COUNT=1
		#Set up the port array
		while [ $PORT_COUNT -le $INSTANCES_PER_QE ]
				do
				((PORT=$PRI_QD_PORT+$PORT_COUNT))
				PORT_ARRAY[$DIRECTORY_VECTOR]=$PORT
				((PORT_COUNT=$PORT_COUNT+1))
				((DIRECTORY_VECTOR=$DIRECTORY_VECTOR+1))
		done
		DIRECTORY_VECTOR=0
		LOOPC=1
		for HOST in "${QE_LIST[@]}"
				do
				while [ $INST_COUNT -le $INSTANCES_PER_QE ]
						do
						D=${DATA_DIRECTORY[$DIRECTORY_VECTOR]}
						PORT=${PORT_ARRAY[$DIRECTORY_VECTOR]}
						QE_PRIMARY_ARRAY[$ARRAY_INDEX]="${HOST} ${D} ${PORT} ${INST_PREFIX}${LOOPC} ${SEG_PREFIX}${LOOPC} ${LOOPC} ${LOOPC}"
						((DIRECTORY_VECTOR=$DIRECTORY_VECTOR+1))
						((INST_COUNT=$INST_COUNT+1))
						((ARRAY_INDEX=$ARRAY_INDEX+1))
						((LOOPC=$LOOPC+1))
				done
				INST_COUNT=1
				DIRECTORY_VECTOR=0
		done
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

MIRROR_CALCULATOR () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		#Function assumptions
		#1. Mirroring is configured on
		#2. All primary instances will be mirrored
		#3. No tail will mirror more than one segment from another host
		if [ $# -ne 4 ]; then
				ERROR_EXIT "[FATAL]:-Incorrect number of parameters supplied, got $# expected 4"
		fi 
		HOST_LIST=($1) #List of tail hostnames in the cluster
		NUM_QE=${#QE_LIST[@]}
		MIRROR_DIR_ARRAY=($2) #The directory mount points for the mirror data i.e. /dbfast3 /dbfast4 etc
		PRI_INST_TAIL=${#MIRROR_DIR_ARRAY[@]}
		((TOTAL_PRI_INST=$NUM_QE*$PRI_INST_TAIL))
		HOST_TO_MIRROR=$3 #The hostname of the primary that needs a mirror
		HOST_OFFSET=$4 #i.e. for 2 instances per tail valid values are 1 or 2
		((MIRROR_DIR_INDEX=$HOST_OFFSET-1))
		HOST_DIR=${MIRROR_DIR_ARRAY[$MIRROR_DIR_INDEX]}
		if [ $HOST_OFFSET -gt $PRI_INST_TAIL ] || [ $HOST_OFFSET -lt 1 ]; then
				ERROR_EXIT "[FATAL]:-Problem with HOST_OFFSET value"
		fi
		INST_COUNT=1
		VECTOR=0
		M_VECTOR=0
		HOST_COUNT=1
		while [ $HOST_COUNT -le $NUM_QE ] #Loop for the number of QEs
				do
				while [ $INST_COUNT -le $PRI_INST_TAIL ] #Loop for the number of instances per tail
						do
						MASTER_HOST_LIST[$M_VECTOR]=${HOST_LIST[$VECTOR]} #Add this host to master array
						((INST_COUNT=$INST_COUNT+1))  #Increment the host counter
						((M_VECTOR=$M_VECTOR+1)) #Increment the master array vector
				done
				((VECTOR=$VECTOR+1)) #Increment the vector into the array	
				((HOST_COUNT=$HOST_COUNT+1)) #Increment the host counter
				INST_COUNT=1 #Reset the inner loop instance counter
		done
		#Get the postional vector of the hostname supplied to be mirrored
		VECTOR=0
		FOUND=0
		for HOST in "${HOST_LIST[@]}"
				do
				if [ $HOST_TO_MIRROR == $HOST ]
						then
						POSITION=$VECTOR
						FOUND=1
				fi
				((VECTOR=$VECTOR+1))
		done
		if [ $FOUND -ne 1 ]
				then
				ERROR_EXIT "[FATAL]:-Could not locate host $HOST_TO_MIRROR" 2
		fi
		((MIRROR_VECTOR=$POSITION+$HOST_OFFSET))
		if [ $MIRROR_VECTOR -ge $NUM_QE ]; then
				((MIRROR_VECTOR=$MIRROR_VECTOR-$NUM_QE))
		fi
		CALC_MIR_DATA=(${QE_LIST[$MIRROR_VECTOR]} $HOST_DIR)
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


PROCESS_QE () {
		##Got here 22/07/2005 17:55
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		HOSTNAME_ARRAY=$1;shift
		DIRECTORY_ARRAY=$1;shift
		PORT_ARRAY=$1;shift
		INST_ARRAY=$1;shift
		SEG_ARRAY=$1;shift
		SEG_NUM_ARRAY=$1;shift
		INST_NUM_ARRAY=$1;shift
		PRIMARY=$1;shift
		IP_ALLOW=$1;shift
		QE_MASK=$1;shift
		CHECK_POINT_SEGMENTS=$1;shift
		ENCODING=$1;shift
		NUMSEGMENTS=$1;shift
		DBID=$1;shift
		CONTENT=$1
		QE_DIR=$DIRECTORY_ARRAY/$INST_ARRAY
		GET_QD_DB_NAME
		LOG_MSG "[INFO]:-Creating QE data directory $QE_DIR"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$INITDB -E $ENCODING -D $QE_DIR" 
		BACKOUT_COMMAND "$TRUSTED_SHELL ${HOSTNAME_ARRAY} \"$RM -rf $QE_DIR\""
		BACKOUT_COMMAND "$ECHO \"removing directory $QE_DIR on $HOSTNAME_ARRAY\""
		BACKOUT_COMMAND "$TRUSTED_SHELL ${HOSTNAME_ARRAY} \"$PG_CTL stop -m immediate -D $QE_DIR\"" 
		BACKOUT_COMMAND "$ECHO \"Shutting down Postgres on $HOSTNAME_ARRAY\""
		LOG_MSG "[INFO]:-Setting QE access permission"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO MPP Specific configuration >> $QE_DIR/pg_hba.conf" 
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO host all all ${IP_ALLOW} trust >> $QE_DIR/pg_hba.conf" 
		LOG_MSG "[INFO]:-Setting default QE port to $PORT_ARRAY"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO port=${PORT_ARRAY} >> $QE_DIR/postgresql.conf" 
		LOG_MSG "[INFO]:-Setting number of redo segments"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO checkpoint_segments=$CHECK_POINT_SEGMENTS >> $QE_DIR/postgresql.conf" 
		LOG_MSG "[INFO]:-Setting query status reporting"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO stats_command_string=true >> $QE_DIR/postgresql.conf"
		LOG_MSG "[INFO]:-Setting listener configuration"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO listen_addresses = '*' >> $QE_DIR/postgresql.conf"

		#Check to see if there is a PG_CONF_ADD_FILE, if so need to add additional lines to postgresql.conf file
		if [ -f $PG_CONF_ADD_FILE ]
				then
				for NEW_PARAM in `$CAT $PG_CONF_ADD_FILE|$TR -s ' '|$TR -d ' '|$GREP -v "^#"`
						do
						LOG_MSG "[INFO]:-Adding $NEW_PARAM to QE postgresql.conf file"
						RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "$ECHO $NEW_PARAM >> $QE_DIR/postgresql.conf"
				done 
		fi
		LOG_MSG "[INFO]:-Starting Functioning instance on QE $HOSTNAME_ARRAY"
		$TRUSTED_SHELL ${HOSTNAME_ARRAY} "export PGPORT=${PORT_ARRAY}; $PG_CTL -w -D $QE_DIR -l ${QE_DIR}.log -o \"-i -p ${PORT_ARRAY}\" start" 
		if [ $? -ne 0 ]; then
				$TRUSTED_SHELL ${HOSTNAME_ARRAY} "$CAT ${QE_DIR}.log "|$TEE -a $LOG_FILE
				ERROR_EXIT "[FATAL]:-Failed to start QE instance database" 2
		fi
		BACKOUT_COMMAND "$TRUSTED_SHELL ${HOSTNAME_ARRAY} \"$RM -f ${QE_DIR}.log\""
		BACKOUT_COMMAND "$ECHO \"Removing tail log file ${QE_DIR}.log on QE $HOSTNAME_ARRAY\""
		LOG_MSG "[INFO]:-Creating database $SEG_ARRAY on $HOSTNAME_ARRAY"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "env PGOPTIONS=\"-c cdb_session_role=utility\" $PSQL -p ${PORT_ARRAY} template1 -c \"create database $SEG_ARRAY\"" 
		LOG_MSG "[INFO]:-Updating the QE cdb catalog"
		RUN_COMMAND_REMOTE ${HOSTNAME_ARRAY} "env PGOPTIONS=\"-c cdb_session_role=utility\" $PSQL -p ${PORT_ARRAY} $SEG_ARRAY -c \"insert into cdb.cdb values ('$CLUSTER_NAME',$NUMSEGMENTS,$DBID,$CONTENT)\""
		LOG_MSG "[INFO]:-Adding this QE instance to the QD host"
		if [ $PRIMARY -eq 1 ]; then
				LOG_MSG "[INFO]:-Adding the primary segment to the head admin schema"
				RUN_COMMAND "$PSQL -p $HEAD_PORT $HEAD_DBNAME -c \"SET search_path TO public;select cdb.add_segment('$SEG_ARRAY','$USER','',$SEG_NUM_ARRAY)\"" 
		else
				LOG_MSG "[INFO]:-Adding secondary service for $SEG_ARRAY to this instance."
				RUN_COMMAND "$PSQL -p $HEAD_PORT $HEAD_DBNAME -c \"insert into cdb_segment_instance values($SEG_NUM_ARRAY,$INST_NUM_ARRAY,true)\""
		fi	
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
#******************************************************************************
# Setup logging directory
#******************************************************************************
CUR_DATE=`$DATE +%Y%m%d`
DEFLOGDIR=`pwd`/logs
if [ ! -d $DEFLOGDIR ]; then
		mkdir $DEFLOGDIR
fi
if [ $LOG_DIR ]; then
		CHK_DIR $LOG_DIR
		if [ $EXISTS -eq 1 ]; then
				ERROR_EXIT "[FATAL]:-Log directory $LOG_DIR does not exist" 2
		fi
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
else
		LOG_FILE=$DEFLOGDIR/${PROG_NAME}_${CUR_DATE}.log
fi

if [ $# -ne 0 ]; then
		if [ $1 == "-v" ]; then
				VERSION_INFO
		fi
fi
