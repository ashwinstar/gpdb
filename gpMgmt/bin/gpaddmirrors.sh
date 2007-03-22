#!/bin/bash
#	Filename:-		gpaddmirrors.bsh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Adds mirror segments to a MPP instance with primary segments only
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
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
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
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
MIRROR_OFFSET=1000
BALANCE=0
MAX_MIR_PORT=61000
MIN_MIR_PORT=7000
PORT_OVERRIDE=0
INTERACTIVE=1
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	if [ -f ${MPPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
		$MORE ${MPPDOCDIR}/$HELP_DOC_NAME
		exit 0
	else
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
	$ECHO "      Configures mirror segments for an MPP Instance which has primary"
	$ECHO "      segment instances only. Process will stop the MPP Instance to be"
	$ECHO "      able to generate mirror copies of primary segments."
        $ECHO
        $ECHO "      Usage:"
        $ECHO "      $0 [OPTIONS]"
        $ECHO
        $ECHO "      General options:"
        $ECHO "      -?, display this help message & exit"
        $ECHO "      -v, display version information & exit"
	$ECHO "      -a, don't ask to confirm mirror creation process"
	$ECHO "          Must supply -m <mirror configuration file> if this option used"
	$ECHO
	$ECHO "      Logging options:"
	$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
	$ECHO "      -D, set log output to debug level, shows all function calls"
	$ECHO
	$ECHO "      Connection options:"
	$ECHO "      -d, <Master instance directory> name of alternative Master instance to add mirrors [optional]"
	$ECHO
	$ECHO "      Mirror creation options:"
	$ECHO "      -p, mirror offset, if not supplied, script will calculate a suitable"
	$ECHO "          port offset for the mirror segment instances, range $MIN_MIR_PORT - $MAX_MIR_PORT"
	$ECHO "          Current offset value is $MIRROR_OFFSET"
	$ECHO "      -s, spread mirrors amongst available MPP hosts [default group mirrors]"
	$ECHO "          Mirror grouping will mirror all primary segments from one host onto"
	$ECHO "          another host within the MPP array."
	$ECHO "          Mirror spreading will place each mirror on a different host within the"
	$ECHO "          MPP array. Spreading will only be allowed if there are a sufficient number"
	$ECHO "          of hosts, i.e. number of segment instance hosts >= number segments/host" 
	$ECHO "      -m, <mirror configuration file> contains details of mirror configuration to"
	$ECHO "          implement for the master instance, file format for each line:"
	$ECHO "          <mirror host name>:<top level segment directory>"
	$ECHO "          <mirror host name> name returned via hostname command. Hostnames must either"
	$ECHO "          match primary segment hostnames, or, be all different from primary segment"
	$ECHO "          hostnames."
	$ECHO "          <top level segment directory> directory name where segment instance directory"
	$ECHO "          will be created."
	$ECHO "          Number of <mirror host name>:<top level segment directory> lines, must match"
	$ECHO "          with number of primary segments per host in master instance"
	$ECHO "          Example contents"
	$ECHO "          host1:/dbfast2/mir"
	$ECHO "          host1:/dbfast4/mir"
	$ECHO "          host2:/dbfast2/mir"
	$ECHO "          host2:/dbfast4/mir"
	$ECHO "          Will create 2 mirror segment instances on hosts host1 and host2, for a primary"
	$ECHO "          instance that consists of 2 hosts with 2 primary instances on each host" 
	$ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
	fi
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
		ERROR_EXIT "[FATAL]:-Mirroring already configured for this MPP Instance" 2
	fi	
	#Check to see if we have a file if -m option supplied
	if [ x"" != x"$MIRROR_MACHINE_LIST_FILE" ];then
		CHK_FILE $MIRROR_MACHINE_LIST_FILE
		if [ $EXISTS -eq 1 ];then
			ERROR_EXIT "[FATAL]:-Issue with $MIRROR_MACHINE_LIST_FILE, please check file" 2
		fi
		#Get line count for file
		MIRROR_COUNT_FILE=`$CAT $MIRROR_MACHINE_LIST_FILE|$WC -l`
		LOG_MSG "[INFO]:-Number of lines in file = $MIRROR_COUNT_FILE"
		#Check that have : field separator
		if [ `$CAT $MIRROR_MACHINE_LIST_FILE|$GREP -c ":"` -ne $MIRROR_COUNT_FILE ];then
			ERROR_EXIT "[FATAL]:-Seperator/line number mismatch, please check file" 2
		else
			LOG_MSG "[INFO]:-Phase I file check complete $MIRROR_MACHINE_LIST_FILE" 1
		fi
		#Check to ensure have no blank hostnames
		if [ `$CAT $MIRROR_MACHINE_LIST_FILE|$CUT -d":" -f1|$CUT -d":" -f1|$TR '\n' ' '|$WC -w` -ne $MIRROR_COUNT_FILE ];then
			ERROR_EXIT "[FATAL]:-There are some hostname fields blank" 2
		else
			LOG_MSG "[INFO]:-Phase II file check complete $MIRROR_MACHINE_LIST_FILE" 1
		fi
		#Check to ensure have no blank directory fields
		if [ `$CAT $MIRROR_MACHINE_LIST_FILE|$CUT -d":" -f2|$CUT -d":" -f1|$TR '\n' ' '|$WC -w` -ne $MIRROR_COUNT_FILE ];then
			ERROR_EXIT "[FATAL]:-There are some directory fields blank" 2
		else
			LOG_MSG "[INFO]:-Phase III file check complete $MIRROR_MACHINE_LIST_FILE" 1
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

OBTAIN_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Obtain number of segment instances per host
	PRI_SEG_PER_HOST=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(distinct port) from $CONFIG_TABLE b where content<>-1 and definedprimary='t';"`
	ERROR_CHK $? "obtain primary segments per host" 2
	MAX_DBID=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(dbid)+1 from $CONFIG_TABLE;"`
	ERROR_CHK $? "obtain current max dbid" 2
	CALC_MIRROR_PORT_BASE
	PRI_HOSTNAME_ARRAY=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c "select distinct hostname from $CONFIG_TABLE where content<>-1 and definedprimary='t' order by hostname;"`
	ERROR_CHK $? "obtain primary hostname list" 2
	BAL_HOSTNAME_ARRAY=$PRI_HOSTNAME_ARRAY" "`$ECHO $PRI_HOSTNAME_ARRAY|$CUT -d" " -f1`
	PRI_HOST_COUNT=`$ECHO $PRI_HOSTNAME_ARRAY|$WC -w`
	if [ $BALANCE -ne 1 ];then
	PRI_SEGMENT_ARRAY=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select datadir, port, hostname, content from $CONFIG_TABLE where content<>-1 order by hostname;"`)
	ERROR_CHK $? "obtain segment instance directory suffix" 2
	else
	PRI_SEGMENT_ARRAY=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select datadir, port, hostname, content from $CONFIG_TABLE where content<>-1 order by port;"`)
	ERROR_CHK $? "obtain segment instance directory suffix" 2
	fi
	PRI_SEGMENT_ARRAY_COUNT=${#PRI_SEGMENT_ARRAY[@]}
	if [ $BALANCE -eq 1 ];then
		if [ $PRI_HOST_COUNT -eq 1 ] || [ $PRI_SEG_PER_HOST -eq $PRI_HOST_COUNT ];then
			LOG_MSG "[FATAL]:-Number of segment hosts must be > number of segments per hosts"  1
			ERROR_EXIT "[FATAL]:-Insufficient MPP hosts for -s mirroring option" 2
		fi
		LOG_MSG "[INFO]:-Sufficient MPP segment hosts for -s option" 1
		BAL_PORT_BASE=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(port)-$PRI_SEG_PER_HOST from $CONFIG_TABLE where content<>-1;"`
		ERROR_CHK $? "obtain current port base" 2
		LOG_MSG "[INFO]:-Obtained current port base = $BAL_PORT_BASE" 1
	fi 
	if [ x"" == x"$MIRROR_MACHINE_LIST_FILE" ];then
		DIR_COUNT=0
		while [ $DIR_COUNT -lt $PRI_SEG_PER_HOST ]
		do
			((COUNTER=$DIR_COUNT+1))
			$ECHO "Enter mirror segment instance directory name $COUNTER of $PRI_SEG_PER_HOST >"
			read MIRROR_DATADIRECTORY[$DIR_COUNT]
			if [ x"" == x"$MIRROR_DATADIRECTORY" ];then
				ERROR_EXIT "[FATAL]:-Null value entered for mirror directory" 2
			fi
			((DIR_COUNT=$DIR_COUNT+1))
		done
		#Check to ensure that we have no duplicate dir names
		if [ `$ECHO ${MIRROR_DATADIRECTORY[@]}|$TR ' ' '\n'|$SORT -u|$WC -l` -ne ${#MIRROR_DATADIRECTORY[@]} ];then
			ERROR_EXIT "[FATAL]:-Duplicate segment instance directory names entered" 2
		fi
	else
		#Check that number of lines in file match number of primay segment instances
		if [ $MIRROR_COUNT_FILE -ne $PRI_SEGMENT_ARRAY_COUNT ];then
			ERROR_EXIT "[INFO]:-Mismatch between number of mirror and primary segments" 2
		fi	
		PRI_SEGMENT_NAMES=(`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where content<>-1;"`)
		ERROR_CHK $? "obtain primary hostnames from $CONFIG_TABLE" 2
		MIR_SEGMENT_NAMES=(`$CAT $MIRROR_MACHINE_LIST_FILE|$CUT -d":" -f1|$SORT -u|$TR '\n' ' '`)
		ALL_MIR_SEGMENT_NAMES=(`$CAT $MIRROR_MACHINE_LIST_FILE|$CUT -d":" -f1|$TR '\n' ' '`)
		#Check to see if all hostnames match or none	
		MIR_HOST_COUNT=${#MIR_SEGMENT_NAMES[@]}
		LOG_MSG "[INFO]:-Obtained mirror host count as $MIR_HOST_COUNT" 1
		MATCH_COUNT=0
		LOG_MSG "[INFO]:-Checking for host name conflicts" 1
		for MIR_HOST in "${MIR_SEGMENT_NAMES[@]}"
		do
			LOG_MSG "[INFO]:-Checking mirror host $MIR_HOST" 1
			if [ `$ECHO ${PRI_SEGMENT_NAMES[@]}|$GREP -c $MIR_HOST` -eq 1 ];then
				((MATCH_COUNT=$MATCH_COUNT+1))
			fi
		done
		if [ $MATCH_COUNT -eq 0 ];then
			LOG_MSG "[INFO]:-Will configure mirror segments on new segment instance hosts" 1
		else
			if [ $MATCH_COUNT -eq $MIR_HOST_COUNT ];then
				LOG_MSG "[INFO]:-Will configure mirror segments on current segment instance hosts" 1
			else
				ERROR_EXIT "[FATAL]:-Mix of current and new mirror segment hosts not allowed" 2
			fi
		fi
		#Set up mirror directory array
		MIRROR_DATADIRECTORY=(`$CAT $MIRROR_MACHINE_LIST_FILE|$CUT -d: -f2|tr '\n' ' '`)
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

OBTAIN_CURRENT_PORT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MAX_PORT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select max(port) from $CONFIG_TABLE where content<>-1;"`
	ERROR_CHK $? "obtain max port number from $CONFIG_TABLE" 2
	MIN_PORT=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select min(port) from $CONFIG_TABLE where content<>-1;"`
	ERROR_CHK $? "obtain min port number from $CONFIG_TABLE" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CALC_MIRROR_PORT_BASE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	OBTAIN_CURRENT_PORT
	if [ $MAX_PORT -ge 40000 ];then
		((MIRROR_PORT_BASE=${MIN_PORT}-$MIRROR_OFFSET-1))
	else
		((MIRROR_PORT_BASE=${MIN_PORT}+$MIRROR_OFFSET-1))
	fi
	#Now to ensure that calculated MIRROR_PORT_BASE lies between MAX_MIRROR_PORT and MIN_MIRROR_PORT
	if [ $MIRROR_PORT_BASE -ge $MAX_MIR_PORT ] || [ $MIRROR_PORT_BASE -le $MIN_MIR_PORT ];then
		LOG_MSG "[FATAL]:-Value of MIRROR_OFFSET supplied via -p option out of range" 1
		LOG_MSG "[FATAL]:-With value supplied, calculated a mirror port base of $MIRROR_PORT_BASE" 1
		ERROR_EXIT "[FATAL]:-Mirror port base range must be between $MIN_MIR_PORT and $MAX_MIR_PORT" 2
	fi
	LOG_MSG "[INFO]:-Setting mirror port base = $MIRROR_PORT_BASE" 1
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
	BALANCE_COUNT=2
	DIR_VECTOR=0
	ARRAY_VECTOR=0
	HOST_COUNT=1
	MIRIDX=0
	USE_BALANCE_ARRAY=(`$ECHO ${PRI_HOSTNAME_ARRAY[@]}|$TR '\n' ' '`)
	((LOOP_MAX=$PRI_SEG_PER_HOST-1))
	((HOST_MAX=$PRI_HOST_COUNT-2))
	MIR_DIR_VECTOR=0
	COUNT=0
	LOOP=0
	MIR_COUNT=0
        while [ $HOST_COUNT -le $PRI_HOST_COUNT ]
        do
                MIRROR_LIST=(${MIRROR_LIST[@]} ${MIRROR_DATADIRECTORY[@]})
                ((HOST_COUNT=$HOST_COUNT+1))
        done
	for PRIDETAILS in "${PRI_SEGMENT_ARRAY[@]}"
	do
		CUR_HOST=`$ECHO $PRIDETAILS|$CUT -d"|" -f3`
		CUR_SEGMENT=`$ECHO $PRIDETAILS|$CUT -d"|" -f1|$AWK -F"/" '{print $NF}'`
		CUR_PORT=`$ECHO $PRIDETAILS|$CUT -d"|" -f2`
		if [ $BALANCE -eq 1 ];then
			CHOICE_ARRAY=(`$ECHO ${USE_BALANCE_ARRAY[@]}|$TR ' ' '\n'|$GREP -v $CUR_HOST|tr '\n' ' '`)
			MIRROR_HOSTNAME=${CHOICE_ARRAY[$LOOP]}
			while [ `$ECHO ${USED_HOSTNAME[@]}|$TR ' ' '\n'|$GREP -c $MIRROR_HOSTNAME` -eq $PRI_SEG_PER_HOST ]
			do
				if [ $LOOP -lt $LOOP_MAX ] || [ $LOOP -lt $HOST_MAX ];then
					((LOOP=$LOOP+1))
				else
					LOOP=0
				fi
				MIRROR_HOSTNAME=${CHOICE_ARRAY[$LOOP]}
			done
			if [ $LOOP -lt $LOOP_MAX ] || [ $LOOP -lt $HOST_MAX ];then
				((LOOP=$LOOP+1))
			else
				LOOP=0
			fi
			USED_HOSTNAME=("${USED_HOSTNAME[@]}" "$MIRROR_HOSTNAME")
			((COUNT=$COUNT+1))
			((MIRROR_PORT=$CUR_PORT+$MIRROR_OFFSET))
			MIRROR_CHK=${MIRROR_HOSTNAME}${MIRROR_DATADIRECTORY[$MIR_COUNT]}
			while [ `$ECHO ${MIRROR_USED[@]}|$TR ' ' '\n'|$GREP -c $MIRROR_CHK` -eq 1 ]
			do
				if [ $MIR_COUNT -eq $LOOP_MAX ];then
					MIR_COUNT=0
				else
					((MIR_COUNT=$MIR_COUNT+1))
				fi 	
				MIRROR_CHK=${MIRROR_HOSTNAME}${MIRROR_DATADIRECTORY[$MIR_COUNT]}
			done	
			MIRROR_DIR=${MIRROR_DATADIRECTORY[$MIR_COUNT]}/$CUR_SEGMENT
			MIRROR_USED=(${MIRROR_USED[@]} ${MIRROR_CHK})
                else
			if [ x"" == x"$MIRROR_MACHINE_LIST_FILE" ];then
				MIRROR_HOSTNAME=`$ECHO $BAL_HOSTNAME_ARRAY|$CUT -d" " -f$BALANCE_COUNT`
			else
				((MIRIDX=$MIRIDX+1))
				MIRROR_HOSTNAME=`$ECHO ${ALL_MIR_SEGMENT_NAMES[@]}|$CUT -d" " -f$MIRIDX`
			fi	
                        ((ARRAY_VECTOR=$ARRAY_VECTOR+1))
                        if [ $ARRAY_VECTOR -eq $PRI_SEG_PER_HOST ];then
                                ((BALANCE_COUNT=$BALANCE_COUNT+1))
                                ARRAY_VECTOR=0
                        fi
			MIRROR_DIR=${MIRROR_LIST[$DIR_VECTOR]}/`$ECHO $PRIDETAILS|$CUT -d"|" -f1|$AWK -F"/" '{print $NF}'`
                fi
		((MIRROR_DBID=$MAX_DBID+$DIR_VECTOR))
		((DIR_VECTOR=$DIR_VECTOR+1))
		MIRROR_CONTENT=`$ECHO $PRIDETAILS|$CUT -d"|" -f4`
		if [ $BALANCE -ne 1 ];then
			PRI_PORT=`$ECHO $PRIDETAILS|$CUT -d"|" -f2`
			((MIRROR_PORT=$MIRROR_OFFSET+$PRI_PORT))
		fi
		if [ $PRI_HOST_COUNT -eq 1 ];then
			if [ x"" == x"$MIRROR_MACHINE_LIST_FILE" ];then
				MIRROR_HOSTNAME=`$ECHO $PRIDETAILS|$CUT -d"|" -f3`
			else
				MIRROR_HOSTNAME=$MIRROR_HOSTNAME
			fi
		fi
		MIRROR_ARRAY=(${MIRROR_ARRAY[@]} $MIRROR_DIR"|"$MIRROR_PORT"|"$MIRROR_DBID"|"$MIRROR_CONTENT"|"$MIRROR_HOSTNAME)
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"	
}

SET_VAR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MIRROR_DATADIR=`$ECHO $MIRROR_SEG|$CUT -d"|" -f1`
	MIRROR_PORT=`$ECHO $MIRROR_SEG|$CUT -d"|" -f2`
	MIRROR_DBID=`$ECHO $MIRROR_SEG|$CUT -d"|" -f3`
	MIRROR_CONTENT=`$ECHO $MIRROR_SEG|$CUT -d"|" -f4`
	MIRROR_HOSTNAME=`$ECHO $MIRROR_SEG|$CUT -d"|" -f5`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_MIRROR_DIR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing host and directory checks, please wait..." 1
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
		SET_VAR $MIRROR_SEG
		LOG_MSG "[INFO]:-Checking if can see $MIRROR_HOSTNAME on network"
		PING_HOST $MIRROR_HOSTNAME
		LOG_MSG "[INFO]:-Checking $MIRROR_HOSTNAME for Data directory $MIRROR_DATADIR"
		CHK_DIR $MIRROR_DATADIR $MIRROR_HOSTNAME
		if [ $EXISTS -eq 0 ];then
			ERROR_EXIT "[FATAL:-Directory $MIRROR_DATADIR already exists on $MIRROR_HOSTNAME" 2
		fi
		W_DIR=`$DIRNAME $MIRROR_DATADIR`
		LOG_MSG "[INFO]:-Checking write access to $W_DIR on Segment host $MIRROR_HOSTNAME"
		$TRUSTED_SHELL $MIRROR_HOSTNAME "$TOUCH $W_DIR/tmp_test_file"
		RETVAL=$?
		if [ $RETVAL -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Cannot write to $W_DIR on Segment host $MIRROR_HOSTNAME" 2
		else
			$TRUSTED_SHELL $MIRROR_HOSTNAME "$RM -f $W_DIR/tmp_test_file"
			LOG_MSG "[INFO]:-Write test passed MIRROR_DATADIR on Segment host $MIRROR_HOSTNAME"
		fi
		#Get the primary hostname
		PRI_HOST=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where content=${MIRROR_CONTENT};"`
		ERROR_CHK $? "obtain primary hostname" 2
		LOG_MSG "[INFO]:-Checking ssh access from $PRI_HOST to $MIRROR_HOSTNAME"
		$SSH $PRI_HOST "$SSH $MIRROR_HOSTNAME \"ls /tmp\"" > /dev/null 2>&1
		ERROR_CHK $? "check inter host ssh access from $PRI_HOST to $MIRROR_HOSTNAME" 2	
	done
	if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $ECHO;fi
	LOG_MSG "[INFO]:-Completed  host and directory checks" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CONFIG_CATALOG () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing shutdown of Master Instance" 1
	if [ -f $GPSTOP ];then
		$GPSTOP -qfa -d $MASTER_DATA_DIRECTORY	
	else
		ERROR_EXIT "[FATAL]:-Could not locate $GPSTOP" 2
	fi
	LOG_MSG "[INFO]:-Starting Master instance in admin mode" 1
	if [ -f $GPSTART ];then
		$GPSTART -qam -d $MASTER_DATA_DIRECTORY
	else
		ERROR_EXIT "[FATAL]:-Could not locate $GPSTART" 2
	fi
	LOG_MSG "[INFO]:-Processing database $DEFAULTDB" 1
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
		env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"insert into $CONFIG_TABLE (content, definedprimary, dbid, isprimary, valid, hostname, port, datadir)  values (${MIRROR_CONTENT},'f',${MIRROR_DBID},'f','t', '${MIRROR_HOSTNAME}',${MIRROR_PORT},'${MIRROR_DATADIR}' );"
		ERROR_CHK $? "insert $CONFIG_TABLE record for $MIRROR_DATADIR port $MIRROR_PORT" 2
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-MPP Instance Mirror Parameters" 1
	LOG_MSG "[INFO]:------------------------------" 1
	LOG_MSG "[INFO]:-MPP Master hostname               = $MASTER_HOSTNAME" 1
	LOG_MSG "[INFO]:-MPP Master data directory         = $MASTER_DATA_DIRECTORY" 1
	LOG_MSG "[INFO]:-MPP Master port                   = $MASTER_PORT" 1
	if [ x"" != x"$MIRROR_MACHINE_LIST_FILE" ];then
	LOG_MSG "[INFO]:-MPP mirror type                   = Separate Hosts" 1
	else
	if [ $BALANCE -eq 0 ];then
	LOG_MSG "[INFO]:-MPP mirror type                   = Group" 1
	else
	LOG_MSG "[INFO]:-MPP mirror type                   = Spread" 1
	fi
	fi
	LOG_MSG "[INFO]:-------------------------------" 1
	LOG_MSG "[INFO]:-Mirror segment Instance List" 1
	LOG_MSG "[INFO]:-------------------------------" 1
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
##		MIRROR_HOSTNAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where dbid=${MIRROR_DBID};"`
	LOG_MSG "[INFO]:-Host $MIRROR_HOSTNAME Segment directory $MIRROR_DATADIR Port $MIRROR_PORT" 1
	done
	if [ $INTERACTIVE ];then
		$ECHO  "Continue with MPP Instance mirror creation Yy/Nn>"
		read REPLY
		CHK_REPLY
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_REPLY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
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
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CREATE_MIRRORS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Commencing data directory synchronization, please wait.." 1
	for MIRROR_SEG in "${MIRROR_ARRAY[@]}"
	do
		SET_VAR $MIRROR_SEG
		MIRROR_HOSTNAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where dbid=${MIRROR_DBID};"`
		ERROR_CHK $? "obtain mirror hostname" 2
		PRIMARY_HOSTNAME=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select hostname from $CONFIG_TABLE where content=${MIRROR_CONTENT} and isprimary='t';"`
		ERROR_CHK $? "obtain primary hostname" 2
		PRIMARY_DATADIR=`env PGOPTIONS="-c gp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"select datadir from $CONFIG_TABLE where content=$MIRROR_CONTENT and dbid<>${MIRROR_DBID};"`
		ERROR_CHK $? "obtain primary data directory for $PRIMARY_HOSTNAME" 2
		LOG_MSG "[INFO]:-Commencing primary data directory copy $PRIMARY_HOSTNAME to $MIRROR_HOSTNAME"
		$TRUSTED_SHELL $PRIMARY_HOSTNAME "$TRUSTED_COPY -pr $PRIMARY_DATADIR ${MIRROR_HOSTNAME}:$MIRROR_DATADIR"
		ERROR_CHK $? "copy data directory on $PRIMARY_HOSTNAME to $MIRROR_HOSTNAME" 2
		ED_FILE ${MIRROR_DATADIR}/$PG_CONF "port=" "port=${MIRROR_PORT}" 0 $MIRROR_HOSTNAME
		LOG_MSG "[INFO]:-Starting mirror segment in admin mode port $MIRROR_PORT $MIRROR_HOSTNAME" 1
		$TRUSTED_SHELL $MIRROR_HOSTNAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -w -D $MIRROR_DATADIR -l ${MIRROR_DATADIR}.log -o \"-i -p $MIRROR_PORT \" start"
		ERROR_CHK $? "start mirror segment port=$MIRROR_PORT host=$MIRROR_HOSTNAME" 2
		LOG_MSG "[INFO]:-Updating $GP_ID_TABLE table for database $DB" 1
		$TRUSTED_SHELL $MIRROR_HOSTNAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PSQL -p $MIRROR_PORT $DEFAULTDB -c \"update $GP_ID_TABLE set dbid=${MIRROR_DBID} where dbid<>${MIRROR_DBID};\""
		ERROR_CHK $? "update $GP_ID_TABLE table on $MIRROR_HOSTNAME $MIRROR_DATADIR database $DB" 2
		LOG_MSG "[INFO]:-Closing down mirror segment from admin mode" 1
		$TRUSTED_SHELL $MIRROR_HOSTNAME "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c gp_session_role=utility\" $PG_CTL -w stop -D $MIRROR_DATADIR -m smart -l ${MIRROR_DATADIR}.log"
		ERROR_CHK $? "close mirror segment $MIRROR_HOSTNAME $MIRROR_DATADIR" 2	
	done
	LOG_MSG "[INFO]:-Completed data directory synchronization" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
        while getopts ":v'?'d:l:p:sam:!D" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                '?' ) USAGE ;;
		! ) SCRIPT_USAGE=1
		    USAGE ;;
                d ) MIRROR_MASTER_DATA_DIRECTORY=$OPTARG ;;
		l ) LOG_DIR=$OPTARG ;;
		p ) MIRROR_OFFSET=$OPTARG ;;
		s ) BALANCE=1 ;;
		a ) unset INTERACTIVE ;;
		m ) MIRROR_MACHINE_LIST_FILE=$OPTARG ;;
		D ) DEBUG_LEVEL=1 ;;
                * ) UNKNOWN_OPTION ;;
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
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" != x"$MIRROR_MASTER_DATA_DIRECTORY" ];then
	MASTER_DATA_DIRECTORY=$MIRROR_MASTER_DATA_DIRECTORY
fi
if [  ! $INTERACTIVE ] && [ x"" == x"$MIRROR_MACHINE_LIST_FILE" ];then
	ERROR_EXIT "[FATAL]:-Can only use -a option if -m <mirror host list file> used" 2
else
	if [ ! $INTERACTIVE ];then
		LOG_MSG "[INFO]:-Non-interactive mirror build from file initiated" 1
	else
		LOG_MSG "[INFO]:-Interactive build initiated" 1
	fi
	if [ $BALANCE -eq 1 ] && [ x"" != x"$MIRROR_MACHINE_LIST_FILE" ];then
		LOG_MSG "[INFO]:-Overriding mirror spread request" 1
		BALANCE=0
	fi
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
	LOG_MSG "[INFO]:-Standby host is configured on the MPP Instance, will sync standby host" 1
	GET_STANDBY_DETAILS
	STANDBY_HOSTNAME=`$ECHO $STANDBY_ARRAY|$CUT -d'|' -f1`
	LOG_MSG "[INFO]:-Obtained standby hostname as $STANDBY_HOSTNAME" 1
	if [ -f $GPINITSTANDBY ];then
		$GPSTOP -qam -d $MASTER_DATA_DIRECTORY
		$GPSTART -qa -d $MASTER_DATA_DIRECTORY
		$GPINITSTANDBY -s $STANDBY_HOSTNAME -n -a
	else
		ERROR_EXIT "[FATAL]:-Could not locate $GPINITSTANDBY" 2
	fi
else
	LOG_MSG "[INFO]:-Commencing shutdown of Master instance from admin mode" 1
	$GPSTOP -qam -d $MASTER_DATA_DIRECTORY
	LOG_MSG "[INFO]:-Commencing startup of mirrored Instance" 1
	$GPSTART -qa -d $MASTER_DATA_DIRECTORY
fi
LOG_MSG "[INFO]:-------------------------------------------------" 1
LOG_MSG "[INFO]:-Successfully configured mirror segment instances" 1
LOG_MSG "[INFO]:-Run gpstate.sh -c to display primary/mirror configuration" 1
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
