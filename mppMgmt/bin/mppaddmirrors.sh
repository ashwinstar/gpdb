#!/bin/bash
#	Filename:-		mppaddmirrors.bsh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Beta testing
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		TBC
#	Release stat:-		Greenplum Co Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Calculates table skew on a per node basis
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code
#******************************************************************************
WORKDIR=`dirname $0`
FUNCTIONS=$WORKDIR/mpp_bash_functions.sh
if [ -f $FUNCTIONS ];then
	. $FUNCTIONS
else
	echo "FATAL:-Cannot source $FUNCTIONS"
	exit 2;fi
#******************************************************************************
# Location Variables
#******************************************************************************

#******************************************************************************
# Command Variables
#******************************************************************************
#
#******************************************************************************
# Script Specific Variables
#******************************************************************************
PROG_NAME=`$BASENAME $0`
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
STOP_PROG=${WORKDIR}/mppstop.sh
START_PROG=${WORKDIR}/mppstart.sh
MIRROR_OFFSET=1000
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
	$ECHO "      Configures mirror segments for an MPP cluster which has primary"
	$ECHO "      segment instances only. Process will stop the MPP cluster to be"
	$ECHO "      able to generate mirror copies of primary segments."
        $ECHO
        $ECHO "      Usage:"
        $ECHO "      $0 [OPTIONS]"
        $ECHO
        $ECHO "      General options:"
        $ECHO "      -?, display this help message & exit"
        $ECHO "      -v, display version information & exit"
	$ECHO
	$ECHO "      Logging options:"
	$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
	$ECHO
	$ECHO "      Mirror creation options:"
	$ECHO "      -p, mirror port base, if not supplied, script will calculate a suitable"
	$ECHO "          port base for the mirror segment instances, range 1000 - 65500"
	$ECHO "      -d, <master data directory> [mandatory]"
	$ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
	$ECHO "Unknown option"
	USAGE
}

CHECK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CHK_DB_RUNNING
	GET_QD_DB_NAME
	#Check to see if mirroring already configured
	CHK_MIRRORS_CONFIGURED
	if [ $MIRROR_COUNT -eq 2 ];then
		ERROR_EXIT "[FATAL]:-Mirroring already configured for this MPP array" 2
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

OBTAIN_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Obtain number of segment instances per host
	PRI_SEG_PER_HOST=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(distinct port) from mpp_configuration b where content<>-1 and definedprimary='t';"`
	ERROR_CHK $? "obtain primary segments per host" 2
	MAX_DBID=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select max(dbid)+1 from mpp_configuration;"`
	#ARRAY_SUPERUSER=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select mppsuperuser from mpp.listener where listenerid=1;"`
	#ERROR_CHK $? "obtain array superuser name" 2
	if [ x"" == x"$MIRROR_PORT_BASE" ];then	
		CALC_MIRROR_PORT_BASE
	else
		CHK_MIRROR_PORT_BASE
	fi
	DIR_COUNT=0
	while [ $DIR_COUNT -lt $PRI_SEG_PER_HOST ]
	do
		((COUNTER=$DIR_COUNT+1))
		$ECHO "Enter mirror segment instance directory $COUNTER of $PRI_SEG_PER_HOST >"
		read MIRROR_DATADIRECTORY[$DIR_COUNT]
		if [ x"" == x"$MIRROR_DATADIRECTORY" ];then
			ERROR_EXIT "[FATAL]:-Null value entered for mirror directory" 2
		fi
		((DIR_COUNT=$DIR_COUNT+1))
	done
	PRI_HOSTNAME_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c "select distinct hostname from mpp_configuration where content<>-1 and definedprimary='t';"`)
	ERROR_CHK $? "obtain primary hostname list" 2
	PRI_HOST_COUNT=${#PRI_HOSTNAME_ARRAY[@]}
	PRI_SEGMENT_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select datadir, port, hostname, content from mpp_configuration where content<>-1 order by dbid;"`)
	ERROR_CHK $? "obtain segment instance directory suffix" 2
	LOG_MSG "[INFO]:-Obtaining list of databases in MPP cluster"
	GET_DB_LIST
	ERROR_CHK $? "obtain database list" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

OBTAIN_CURRENT_PORT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MAX_PORT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select max(port) from mpp_configuration;"`
	ERROR_CHK $? "obtain max port number from mpp_configuration" 2
	MIN_PORT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select min(port) from mpp_configuration;"`
	ERROR_CHK $? "obtain min port number from mpp_configuration" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CALC_MIRROR_PORT_BASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	OBTAIN_CURRENT_PORT
	if [ $MAX_PORT -ge 40000 ];then
		((MIRROR_PORT_BASE=${MIN_PORT}-$MIRROR_OFFSET))
	else
		((MIRROR_PORT_BASE=${MIN_PORT}+$MIRROR_OFFSET))
	fi
	LOG_MSG "[INFO]:-Setting mirror port base = $MIRROR_PORT_BASE"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_MIRROR_PORT_BASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	OBTAIN_CURRENT_PORT
	if [ $MIRROR_PORT_BASE -gt 65500 ];then
		ERROR_EXIT "[FATAL]:-Value of Mirror Port Base too large at $MIRROR_PORT_BASE" 2
	fi
	if [ $MIRROR_PORT_BASE -lt 1000 ];then
		 ERROR_EXIT "[FATAL]:-Value of Mirror Port Base too small at $MIRROR_PORT_BASE" 2
	fi
	#Now check to ensure that port range not in use
	if [ $MIRROR_PORT_BASE -le $MAX_PORT ] && [ $MIRROR_PORT_BASE -ge $MIN_PORT ];then
		ERROR_EXIT "[FATAL]:-Value of Mirror Port clashes with current primary port numbers" 2
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

POSTGRES_PORT_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_PG_PID_ACTIVE $1 $2
	if [ $PID -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Host $2 has an active database process on port = $1" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_MIRROR_ARRAY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	HOST_COUNT=1
	DIR_VECTOR=0
	for PRIDETAILS in "${PRI_SEGMENT_ARRAY[@]}"
	do
		HOSTID_LIST=$HOSTID_LIST" `$ECHO $PRIDETAILS|$CUT -d"|" -f3`"
	done
	while [ $HOST_COUNT -le $PRI_HOST_COUNT ]
	do
		MIRROR_LIST=(${MIRROR_LIST[@]} ${MIRROR_DATADIRECTORY[@]}) 
		((HOST_COUNT=$HOST_COUNT+1))
	done
	HOSTID_INDEX=1	
	for PRIDETAILS in "${PRI_SEGMENT_ARRAY[@]}"
	do
		MIRROR_DIR=${MIRROR_LIST[$DIR_VECTOR]}/`$ECHO $PRIDETAILS|$CUT -d"|" -f1|$AWK -F"/" '{print $NF}'`
		PRI_HOSTID=`$ECHO $PRIDETAILS|$CUT -d"|" -f3`
		MIRROR_HOSTID=`$ECHO $HOSTID_LIST|$TR ' ' '\n'|$SORT -u|$GREP -v $PRI_HOSTID|$TR '\n' ' '|$CUT -d" " -f${HOSTID_INDEX}`
		if [ x"" == x"$MIRROR_HOSTID" ];then
			MIRROR_HOSTID=`$ECHO $HOSTID_LIST|$CUT -d" " -f${HOSTID_INDEX}`;fi
		if [ $HOSTID_INDEX -lt $PRI_SEG_PER_HOST ];then ((HOSTID_INDEX=$HOSTID_INDEX+1));else HOSTID_INDEX=1;fi
		((MIRROR_DBID=$MAX_DBID+$DIR_VECTOR))
		((DIR_VECTOR=$DIR_VECTOR+1))
###		((MIRROR_PORT=`$ECHO $PRIDETAILS|$CUT -d"|" -f2`+$MIRROR_PORT_BASE))
		((MIRROR_PORT=$MIRROR_PORT_BASE+$HOSTID_INDEX))
		MIRROR_CONTENT=`$ECHO $PRIDETAILS|$CUT -d"|" -f4`
		MIRROR_ARRAY=(${MIRROR_ARRAY[@]} $MIRROR_DIR"|"$MIRROR_PORT"|"$MIRROR_DBID"|"$MIRROR_CONTENT"|"$MIRROR_HOSTID"|"$PRI_HOSTID)
		MIRROR_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname from mpp_configuration where hostid=${MIRROR_HOSTID};"`
		POSTGRES_PORT_CHK $MIRROR_PORT $MIRROR_HOSTNAME
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"	
}

SET_VAR () {
	MIRROR_DATADIR=`$ECHO $MIRROR_SEG|$CUT -d"|" -f1`
	MIRROR_PORT=`$ECHO $MIRROR_SEG|$CUT -d"|" -f2`
	MIRROR_DBID=`$ECHO $MIRROR_SEG|$CUT -d"|" -f3`
	MIRROR_CONTENT=`$ECHO $MIRROR_SEG|$CUT -d"|" -f4`
	MIRROR_HOSTID=`$ECHO $MIRROR_SEG|$CUT -d"|" -f5`
	MIRROR_PRI_HOSTID=`$ECHO $MIRROR_SEG|$CUT -d"|" -f6`
}

CHK_MIRROR_DIR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
		LOG_MSG "[INFO]:-Checking $MIRROR_HOSTNAME for Data directory $MIRROR_DATADIR"
		CHK_DIR $MIRROR_DATADIR $MIRROR_HOSTNAME
		if [ $EXISTS -eq 0 ];then
			ERROR_EXIT "[FATAL:-Directory $MIRROR_DATADIR already exists on $MIRROR_HOSTNAME" 2
		fi
	done

	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CONFIG_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing shutdown of Master cluster"
	if [ -f $STOP_PROG ];then
		$STOP_PROG -qa -d $MASTER_DATA_DIRECTORY	
	else
		ERROR_EXIT "[FATAL]:-Could not locate $STOP_PROG" 2
	fi
	LOG_MSG "[INFO]:-Starting Master instance in admin mode"
	if [ -f $START_PROG ];then
		$START_PROG -qam -d $MASTER_DATA_DIRECTORY
	else
		ERROR_EXIT "[FATAL]:-Could not locate $START_PROG" 2
	fi
	DB_LIST=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select datname from pg_database where datallowconn<>'f' and datname<>'postgres';"`)
	ERROR_CHK $? "obtain list of database names from master instance" 2
	for MPP_DATABASE in "${DB_LIST[@]}"
	do
		LOG_MSG "[INFO]:-Processing database $MPP_DATABASE"
		for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
		do
			SET_VAR $MIRROR_SEG
			#env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $MPP_DATABASE -A -t -c"insert into mpp.listener values (${MIRROR_DBID},${MIRROR_HOSTID},${MIRROR_PORT},'${MIRROR_DATADIR}','${ARRAY_SUPERUSER}');"
			#ERROR_CHK $? "insert listener record for $MIRROR_DATADIR port $MIRROR_PORT" 2
			env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $MPP_DATABASE -A -t -c"insert into mpp_configuration (dbid, isprimary, content, valid, definedprimary, hostname, port, datadir)  values (${MIRROR_DBID},'f',${MIRROR_CONTENT},'t','f', '${MIRROR_HOSTNAME}',${MIRROR_PORT},'${MIRROR_DATADIR}' );"
			ERROR_CHK $? "insert pgdatabase record for $MIRROR_DATADIR port $MIRROR_PORT" 2
		done	
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP Cluster Mirror Parameters"
	LOG_MSG "[INFO]:------------------------------"
	LOG_MSG "[INFO]:-MPP Master hostname               = $MASTER_HOSTNAME"
	LOG_MSG "[INFO]:-MPP Master data directory         = $MASTER_DATA_DIRECTORY"
	LOG_MSG "[INFO]:-MPP Master port                   = $MASTER_PORT"
	LOG_MSG "[INFO]:-------------------------------"
	LOG_MSG "[INFO]:-Mirror segment Instance List"
	LOG_MSG "[INFO]:-------------------------------"
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
		MIRROR_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname from mpp.host where hostid=${MIRROR_HOSTID};"`
	LOG_MSG "[INFO]:-Host=$MIRROR_HOSTNAME Data directory=$MIRROR_DATADIR Port=$MIRROR_PORT"
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	$ECHO  "Continue with MPP mirror creation Yy/Nn>"
	read REPLY
	CHK_REPLY
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_REPLY () {
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
}

CREATE_MIRRORS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing data directory synchronization, please wait.."
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
		MIRROR_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname from mpp.host where hostid=${MIRROR_HOSTID};"`
		ERROR_CHK $? "obtain mirror hostname" 2
		PRIMARY_HOSTNAME=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname from mpp.host where hostid=${MIRROR_PRI_HOSTID};"`
		ERROR_CHK $? "obtain primary hostname" 2
		LOG_MSG "[INFO]:-Commencing primary data directory copy $PRIMARY_HOSTNAME to $MIRROR_HOSTNAME"
		PRIMARY_DATADIR=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select datadir from mpp_configuration where content=$MIRROR_CONTENT and dbid<>${MIRROR_DBID};"`
		ERROR_CHK $? "obtain primary data directory for $PRIMARY_HOSTNAME" 2
		$TRUSTED_SHELL $PRIMARY_HOSTNAME "$TRUSTED_COPY -pr $PRIMARY_DATADIR ${MIRROR_HOSTNAME}:$MIRROR_DATADIR"
		ERROR_CHK $? "copy data directory on $PRIMARY_HOSTNAME to $MIRROR_HOSTNAME" 2
		ED_FILE ${MIRROR_DATADIR}/$PG_CONF "port=" "port=${MIRROR_PORT}" 0 $MIRROR_HOSTNAME
		LOG_MSG "[INFO]:-Starting mirror segment in admin mode"
		$TRUSTED_SHELL $MIRROR_HOSTNAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -w -D $MIRROR_DATADIR -l ${MIRROR_DATADIR}.log -o \"-i -p $MIRROR_PORT \" start"
		ERROR_CHK $? "update segment mpp_mpp on $MIRROR_HOSTNAME DBID=$MIRROR_DBID port=$MIRROR_PORT" 2
		for DB in "${DB_ARRAY[@]}"
		do
			LOG_MSG "[INFO]:-Updating mpp_mpp table for database $DB"
			$TRUSTED_SHELL $MIRROR_HOSTNAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $MIRROR_PORT $DB -c \"update mpp_mpp set dbid=${MIRROR_DBID} where dbid<>${MIRROR_DBID};\""
			ERROR_CHK $? "update mpp_mpp table on $MIRROR_HOSTNAME $MIRROR_DATADIR database $DB" 2
		done
		LOG_MSG "[INFO]:-Closing down mirror segment from admin mode"
		$TRUSTED_SHELL $MIRROR_HOSTNAME "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS=\"-c mpp_session_role=utility\" $PG_CTL -w stop -D $MIRROR_DATADIR -m smart -l ${MIRROR_DATADIR}.log"
		ERROR_CHK $? "close mirror segment $MIRROR_HOSTNAME $MIRROR_DATADIR" 2	
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
        while getopts ":v'?'d:l:p:" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                '?' ) USAGE ;;
                d ) MIRROR_MASTER_DATA_DIRECTORY=$OPTARG ;;
		l ) LOG_DIR=$OPTARG ;;
		p ) MIRROR_PORT_BASE=$OPTARG ;;
                * ) UNKNOWN_OPTION ;;
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
if [ x"" == x"$MIRROR_MASTER_DATA_DIRECTORY" ];then
	ERROR_EXIT "[FATAL]:-Must supply -d <MASTER_DATA_DIRECTORY> option"
else
	MASTER_DATA_DIRECTORY=$MIRROR_MASTER_DATA_DIRECTORY
fi
CHK_ON_PASSIVE_STANDBY
CHECK_PARAM
OBTAIN_PARAM
CREATE_MIRROR_ARRAY
CHK_MIRROR_DIR
DISPLAY_PARAM
CONFIG_CATALOG
CREATE_MIRRORS
GET_STANDBY_COUNT
if [ $STANDBY_COUNT -ne 0 ];then
	LOG_MSG "[INFO]:-Standby host is configured on the MPP cluster, will sync standby host"
	GET_STANDBY_DETAILS
	STANDBY_HOSTNAME=`$ECHO $STANDBY_ARRAY|$CUT -d'|' -f1`
	LOG_MSG "[INFO]:-Obtained standby hostname as $STANDBY_HOSTNAME"
	if [ -f $WORKDIR/mppinitstandby.sh ];then
		$STOP_PROG -qam -d $MASTER_DATA_DIRECTORY
		$START_PROG -qa -d $MASTER_DATA_DIRECTORY
		$WORKDIR/mppinitstandby.sh -s $STANDBY_HOSTNAME -n -i -a
	else
		ERROR_EXIT "[FATAL]:-Could not locate $WORKDIR/mppinitstandby.sh" 2
	fi
else
	LOG_MSG "[INFO]:-Commencing shutdown of Master instance from admin mode"
	$STOP_PROG -qam -d $MASTER_DATA_DIRECTORY
	LOG_MSG "[INFO]:-Commencing startup of mirrored cluster"
	$START_PROG -qa -d $MASTER_DATA_DIRECTORY
fi
LOG_MSG "[INFO]:-Successfully configured mirror segment instances"
LOG_MSG "[INFO]:-Run mppstate.sh -c to display primary/mirror configuration"
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
