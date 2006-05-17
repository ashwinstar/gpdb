#!/bin/bash
#	Filename:-		mppcrondump.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Script that can be called from cron or command line to dump a database.
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
EXIT_STATUS=0
DUMP_TYPE=0
CLEAR_DUMPS=0
COMPRESSION=1
FAILED_PRIMARY_COUNT=0
FAILED_MIRROR_COUNT=0
DUMP_SUB_DIR=db_dumps
COMPRESSION_FACTOR=7
FREE_SPACE_PERCENT=10
ROLL_BACK=0
RUN_CLEAR_DUMPS=0
CUR_HOST=`$HOSTNAME`
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
		$ECHO	
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Dumps a MPP Instance database using mpp_dump utility"
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
		$ECHO "      -a, don't ask to confirm database dump [default:- ask]"
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master instance directory> name of alternative Master instance to start up [optional]"
		$ECHO
		$ECHO "      Database dump options:"
		$ECHO "      -p, dump primary segment instances only [default dump primary and mirror]"
		$ECHO "      -m, dump mirror segment instances only [default dump primary and mirror]"
		$ECHO "      -c, clear old dump directories [default do not clear]"
		$ECHO "      -o, clear dump files only, do not run a dump"
		$ECHO "      -z, do not use compression [default use compression]"
		$ECHO "      -x, database name to dump [mandatory]"
		$ECHO "      -r, do not rollback dump files if dump failure detected [default rollback]"
		$ECHO "      -f, % free disk value to reserve when calculating available free space for dump"
		$ECHO "          files [default $FREE_SPACE_PERCENT %]"
		$ECHO "      -b, bypass disk space checking [default check disk space]"
		$ECHO "      -i, ignore initial parameter check phase [default check parameters]"
		$ECHO "      -s, <schema name> dump the schema contained within the database name supplied via -x"
		$ECHO
		$ECHO "      Crontab entry (example):"
		$ECHO "      SHELL=/bin/bash"
		$ECHO "      5 0 * * * . \$HOME/.bashrc;\$MPPUTIL/mppcrondump.sh -x tpch -aq >> <name of cronout file>" 
		$ECHO "      Set the shell to /bin/bash (default for cron is /bin/sh"
		$ECHO "      Dump the tpch database, start process 5 minutes past midnight on a daily basis"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but dump completed"
		$ECHO "      2 Fatal error, dump failed"
		$ECHO
		exit $EXIT_STATUS
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

CHK_QD_DB_RUNNING () {
		if [  -s $MASTER_DATA_DIRECTORY/$PG_PID ]; then
			LOG_MSG "[INFO]:-$PG_PID file exists on Master" 
		else
			ERROR_EXIT "[FATAL]:-No Master segment $PG_PID"
		fi
}

GET_DUMP_LIST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	case $DUMP_TYPE in
		0) LOG_MSG "[INFO]:-Gathering dbid data for dump type all"
		   DBID_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where content<>-1;"`)  ;;
		1) LOG_MSG "[INFO]:-Gathering dbid data for dump type primary" 
		   DBID_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where content<>-1 and definedprimary='t';"`)  ;;
	
		2) LOG_MSG "[INFO]:-Gathering dbid data for dump type mirror"
		   DBID_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where content<>-1 and definedprimary='f';"`)  ;;
		3) LOG_MSG "[WARN]:-Gathering dbid data for dump type available active" 
		   DBID_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where content<>-1 and isprimary='t';"`) ;;
	esac
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		CHK_DIR_TARGET=`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select hostname, datadir from $CONFIG_TABLE where dbid=${DBID};"`
		ERROR_CHK $? "obtained host data for DBID $DBID" 2
		DIR_HOST=`$ECHO $CHK_DIR_TARGET|$CUT -d"|" -f1`
		DIR=`$ECHO $CHK_DIR_TARGET|$CUT -d"|" -f2`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_DUMP_DIR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for DBID in ${DBID_LIST[@]}
	do
		CHK_DATA
		LOG_MSG "[INFO]:-Checking for $DUMP_SUB_DIR for dbid = $DBID"
		CHK_DIR ${DIR}/$DUMP_SUB_DIR $DIR_HOST
		if [ $EXISTS -ne 0 ];then
			LOG_MSG "[INFO]:-Directory ${DIR}/$DUMP_SUB_DIR not found on $DIR_HOST, will try to create"
			$TRUSTED_SHELL $DIR_HOST "$MKDIR ${DIR}/$DUMP_SUB_DIR"
			ERROR_CHK $? "make dump directory ${DIR}/$DUMP_SUB_DIR on $DIR_HOST" 2
		else
			LOG_MSG "[INFO]:-Directory ${DIR}/$DUMP_SUB_DIR exists on $DIR_HOST"
			$TRUSTED_SHELL $DIR_HOST "$TOUCH ${DIR}/$DUMP_SUB_DIR/tmp_file_test"
                        RETVAL=$?
                        if [ $RETVAL -ne 0 ];then
                        	ERROR_EXIT "[FATAL]:-Cannot write to ${DIR}/$DUMP_SUB_DIR on $DIR_HOST " 2
                        else
                        	$TRUSTED_SHELL $DIR_HOST "$RM -f ${DIR}/$DUMP_SUB_DIR/tmp_file_test"
                        	LOG_MSG "[INFO]:-Write test passed on $DIR_HOST ${DIR}/$DUMP_SUB_DIR directory"
                        fi
		fi
	done	
	LOG_MSG "[INFO]:-Checking for $DUMP_SUB_DIR on Master segment"
	CHK_DIR $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR
	if [ $EXISTS -ne 0 ];then
		LOG_MSG "[INFO]:-$MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR does not exist, will try to create"
		$MKDIR $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR
		ERROR_CHK $? "make dump directory $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR on Master segment" 2
	else
		LOG_MSG "[INFO]:-Directory $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR exists on Master segment"
		$TOUCH $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR/tmp_file_test
		RETVAL=$?
		if [ $RETVAL -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Cannot write to $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR" 2
		else
			$RM -f $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR/tmp_file_test
			LOG_MSG "[INFO]:-Write test passed on $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR"
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DUMP_MIR_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	DB_CHK=`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from pg_database where datname='${DUMP_DATABASE}';"`
	if [ $DB_CHK -eq 0 ];then
		ERROR_EXIT "[FATAL]:-Database $DUMP_DATABASE does not exist" 2
	fi
	CHK_MIRRORS_CONFIGURED
	if [ $MIRROR_COUNT -ne 2 ];then
		if [ $DUMP_TYPE -eq 2 ];then
			LOG_MSG "[WARN]:-Option -m supplied, but MPP instance not mirrored"
			LOG_MSG "[INFO]:-Set dump type to primary"
			DUMP_TYPE=1
			EXIT_STATUS=1
		fi
		if [ $DUMP_TYPE -eq 0 ];then
			LOG_MSG "[WARN]:-Default option for primary and mirror segments invalid, MPP instance not mirrored"
			LOG_MSG "[INFO]:-Set dump type to primary"
			DUMP_TYPE=1
			EXIT_STATUS=1
		fi
	fi 
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}	
CHK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check to see if there are any segment failures
	if [ x"" != x"$SCHEMA_NAME" ];then
		LOG_MSG "[INFO]:-Schema option -n $SCHEMA_NAME supplied, checking for schema in database $DUMP_DATABASE"
		SCHEMA_COUNT=`$PSQL -p $MASTER_PORT $DUMP_DATABASE -R" " -A -t -c"select count(*) from pg_namespace where nspname='${SCHEMA_NAME}';"`
		if [ $SCHEMA_COUNT -eq 0 ];then
			ERROR_EXIT "[FATAL]:-Schema $SCHEMA_NAME does not exist in database $DUMP_DATABASE" 2
		fi
	fi
	FAILED_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from $MPP_PG_VIEW where valid='f';"`
	if [ $FAILED_COUNT -eq 0 ];then
		LOG_MSG "[INFO]:-No failed segment instances detected"
	else
		LOG_MSG "[WARN]:-Failed segment instances detected, analyzing dump options"
		FAILED_MIRROR_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from $MPP_PG_VIEW where content <> -1 and definedprimary='f' and valid='f';"`
		ERROR_CHK $? "obtained failed mirror count" 2
		FAILED_PRIMARY_COUNT=`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select count(*) from $MPP_PG_VIEW where content <> -1 and definedprimary='t' and valid='f';"`
		ERROR_CHK $? "obtained failed primary count" 2
		if [ $FAILED_MIRROR_COUNT -ne 0 ] && [ $FAILED_PRIMARY_COUNT  -ne 0 ];then
			LOG_MSG "[WARN]:-Have detected both primary and mirror segment failures"
			LOG_MSG "[INFO]:-Configuring options to dump current active primary segments"
			EXIT_STATUS=1
			DUMP_TYPE=3
		fi
		if [ $FAILED_MIRROR_COUNT -ne 0 ] && [ $DUMP_TYPE -eq 2 ];then	
			LOG_MSG "[WARN]:-Detected failed mirror count of $FAILED_MIRROR_COUNT, -m option changed to -p"
			EXIT_STATUS=1
			DUMP_TYPE=1
		fi
		if [ $FAILED_PRIMARY_COUNT -ne 0 ] && [ $DUMP_TYPE -eq 1 ];then
			LOG_MSG "[WARN]:-Detected failed primary count of $FAILED_PRIMARY_COUNT, -p option changed to -m" 
			EXIT_STATUS=1
			DUMP_TYPE=2
		fi
		if [ $FAILED_MIRROR_COUNT -ne 0 ] && [ $DUMP_TYPE -eq 0 ];then
			LOG_MSG "[WARN]:-Detected failed mirror count of $FAILED_MIRROR_COUNT, will dump active segments"
			EXIT_STATUS=1
			DUMP_TYPE=3
		fi
		if [ $FAILED_PRIMARY_COUNT -ne 0 ] && [ $DUMP_TYPE -eq 0 ];then
			LOG_MSG "[WARN]:-Detected failed primary count of $FAILED_PRIMARY_COUNT, will dump active segments"
			EXIT_STATUS=1
			DUMP_TYPE=3
		fi
	fi
		GET_DUMP_LIST	
		CHK_DUMP_DIR
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_DUMP_SPACE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check to see if we have sufficient space to be able to dump data to disks
	LOG_MSG "[INFO]:-Obtaining $DUMP_DATABASE disk allocation"
	ACTUAL_DB_SIZE=(`$MPPSIZECALC -x $DUMP_DATABASE -fq|$GREP '|'`)
	LOG_MSG "[INFO]:-Checking free space on segment instances"	
	for DBID in ${DBID_LIST[@]}
	do
		CHK_DATA
		DIR_HOST_FREE_K=`$TRUSTED_SHELL $DIR_HOST "$DF -k $DIR|$TAIL -1|$AWK '{print \\$4}'"`
		LOG_MSG "[INFO]:-Host $DIR_HOST Directory $DIR has free space $DIR_HOST_FREE_K K"
		SEG=`$BASENAME $DIR`
		DIR_HOST_USED_K=`$ECHO ${ACTUAL_DB_SIZE[@]}|$GREP $DIR_HOST|$GREP $SEG|$AWK -F'|' '{print $NF}'`
		LOG_MSG "[INFO]:-Host $DIR_HOST Directory $DIR Database $DUMP_DATABASE has used $DIR_HOST_USED_K K"
		if [ $COMPRESSION ];then
			((REQ_FREE_SPACE_K=$DIR_HOST_USED_K/$COMPRESSION_FACTOR))
			((CALC1=$DIR_HOST_FREE_K*$FREE_SPACE_PERCENT))
			((CALC2=$CALC1/100))
			((CALC_FREE_SPACE_K=$DIR_HOST_FREE_K-$CALC2))
			LOG_MSG "[INFO]:-Revised space $FREE_SPACE_PERCENT % = $CALC_FREE_SPACE_K K Required $REQ_FREE_SPACE_K K (Estimated size for compressed file)"
			if [ $CALC_FREE_SPACE_K -lt $REQ_FREE_SPACE_K ];then
				ERROR_EXIT "[FATAL]:-Insufficient free space $DIR_HOST $DIR required $REQ_FREE_SPACE_K K actual $CALC_FREE_SPACE_K K" 2
			else
				LOG_MSG "[INFO]:-Sufficient free space on $DIR_HOST for dump in segment directory $DIR"
			fi
		else
			REQ_FREE_SPACE_K=$DIR_HOST_USED_K
			((CALC1=$DIR_HOST_FREE_K*$FREE_SPACE_PERCENT))
			((CALC2=$CALC1/100))
			((CALC_FREE_SPACE_K=$DIR_HOST_FREE_K-$CALC2))
			LOG_MSG "[INFO]:-Revised space $FREE_SPACE_PERCENT % = $CALC_FREE_SPACE_K K Required $REQ_FREE_SPACE_K K "
			if [ $CALC_FREE_SPACE_K -lt $REQ_FREE_SPACE_K ];then
				ERROR_EXIT "[FATAL]:-Insufficient free space $DIR_HOST $DIR required $REQ_FREE_SPACE_K K actual $CALC_FREE_SPACE_K K" 2
			else
				LOG_MSG "[INFO]:-Sufficient free space on $DIR_HOST for dump in segment directory $DIR"
			fi
			
		fi 
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SET_DUMP_TYPE_TEXT () {
	case $DUMP_TYPE in
		0 ) TYPE="Primary and Mirror segments" ;;
		1 ) TYPE="Primary segments only" ;;
		2 ) TYPE="Mirror segments only" ;;
		3 ) TYPE="Available active segments" ;;
	esac
}

PROMPT_CONTINUE () {
		LOG_MSG "[INFO]:-Master MPP Instance dump parameters:"
		LOG_MSG "[INFO]:-------------------------------------"
		LOG_MSG "[INFO]:-Database to be dumped      = $DUMP_DATABASE"
		if [ x"" != x"$SCHEMA_NAME" ];then
		LOG_MSG "[INFO]:-Schema to dump             = $SCHEMA_NAME"
		fi
		LOG_MSG "[INFO]:-Master Port                = $MASTER_PORT"
		LOG_MSG "[INFO]:-Master directory           = $MASTER_DATA_DIRECTORY"
		SET_DUMP_TYPE_TEXT
		LOG_MSG "[INFO]:-MPP Instance dump type     = $TYPE"
		if [ $FAILED_PRIMARY_COUNT -ne 0 ];then
			LOG_MSG "[WARN]:-Failed primary count       = $FAILED_PRIMARY_COUNT"
		else
			LOG_MSG "[INFO]:-Failed primary count       = $FAILED_PRIMARY_COUNT"
		fi
		if [ $FAILED_MIRROR_COUNT -ne 0 ];then
			LOG_MSG "[WARN]:-Failed mirror count        = $FAILED_MIRROR_COUNT"
		else
			LOG_MSG "[INFO]:-Failed mirror count        = $FAILED_MIRROR_COUNT"
		fi	
		if [ $ROLL_BACK -eq 0 ];then
			LOG_MSG "[INFO]:-Rollback dumps             = On"
		else
			LOG_MSG "[INFO]:-Rollback dumps             = Off"
		fi
		if [ $COMPRESSION ];then
			LOG_MSG "[INFO]:-Dump file compression      = On"
		else
			LOG_MSG "[INFO]:-Dump file compression      = Off"
		fi
		if [ $CLEAR_DUMPS -eq 1 ];then
			LOG_MSG "[INFO]:-Clear old dump files       = On"
		else
			LOG_MSG "[INFO]:-Clear old dump files       = Off"
		fi	
		LOG_MSG "[INFO]:-Disk free percent value    = $FREE_SPACE_PERCENT %"
		$ECHO
		$ECHO "Continue with MPP instance dump yY|nN >"
		read REPLY
		if [ -z $REPLY ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!"
				exit 1
		fi
}	

PROCESS_DUMP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Building dump command line"
	DUMP_TEXT="$MPP_DUMP -p $MASTER_PORT -U $LOGNAME --mpp-d=${DUMP_SUB_DIR}/$CUR_DATE --mpp-r=${MASTER_DATA_DIRECTORY}/${DUMP_SUB_DIR}/$CUR_DATE --mpp-s=i[1,`$ECHO ${DBID_LIST[@]}|$TR ' ' ','`]"		
	if [ $COMPRESSION ];then
		LOG_MSG "[INFO]:-Adding compression parameter"
		DUMP_TEXT=$DUMP_TEXT" --mpp-c"
	fi
	if [ x"" != x"$SCHEMA_NAME" ];then
		LOG_MSG "[INFO]:-Adding schema name $SCHEMA_NAME"
		DUMP_TEXT=$DUMP_TEXT" -s $SCHEMA_NAME"
	fi
	DUMP_TEXT=$DUMP_TEXT" $DUMP_DATABASE"
	LOG_MSG "[INFO]:-Dump command line $DUMP_TEXT"
	LOG_MSG "[INFO]:-Starting dump process"
	TIME_START=`$DATE +%H":"%M":"%S`
	$DUMP_TEXT
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		LOG_MSG "[WARN]:-Dump process returned exit code $RETVAL"
	else
		LOG_MSG "[INFO]:-Dump process returned exit code $RETVAL"
	fi
	TIME_END=`$DATE +%H":"%M":"%S`
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_POST_DUMP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Get the latest report file name on Master segment instance
	MASTER_DUMP_RPT=`$LS -altr ${MASTER_DATA_DIRECTORY}/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_*.rpt|$TAIL -1 |$AWK '{print \$NF}'`
	if [ x"" == x"$MASTER_DUMP_RPT" ];then
		LOG_MSG "[WARN]:-Could not locate a report file on Master segment dump directory"
		EXIT_STATUS=1
		LOG_MSG "[INFO]:-Checking to see if can locate a status file on dbid ${DBID_LIST[1]}"
		DBID=${DBID_LIST[1]}
		CHK_DATA
		EXISTS=`$TRUSTED_SHELL $DIR_HOST "if [ -f $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_status_?_${DBID}_$CUR_DATE* ];then $ECHO 0;else $ECHO 1;fi"`
		if [ $EXISTS -eq 0 ];then
			LOG_MSG "[INFO]:-Located dump status file on $DIR_HOST"
			TIMESTAMP_KEY=`$TRUSTED_SHELL $DIR_HOST "$LS $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_status_?_${DBID}_$CUR_DATE*|$CUT -d'_' -f7"`
			LOG_MSG "[INFO]:-Time stamp key = $TIMESTAMP_KEY"
		else
			LOG_MSG "[WARN]:-Failed to locate a dump status file on $DIR_HOST in directory $DIR"
		fi
	else	
		LOG_MSG "[INFO]:-Obtained latest dump report name $MASTER_DUMP_RPT"
		TIMESTAMP_KEY=`$GREP "Timestamp Key:" $MASTER_DUMP_RPT|$AWK '{print \$NF}'`
		LOG_MSG "[INFO]:-Time stamp key = $TIMESTAMP_KEY"
	fi
	for DBID in ${DBID_LIST[@]}
	do
		CHK_DATA
		LOG_MSG "[INFO]:-Checking dump status for $DIR_HOST segment directory $DIR/$DUMP_SUB_DIR/$CUR_DATE"
		EXISTS=`$TRUSTED_SHELL $DIR_HOST "if [ -f $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_status_0_${DBID}_$TIMESTAMP_KEY  ];then $ECHO 0;else $ECHO 1;fi"`
		if [ $EXISTS -eq 1 ];then
			LOG_MSG "[WARN]:-Could not locate status report file"
			ROLLBACK_DUMPS=1
			EXIT_STATUS=1
			FAILED_REPORT=(${FAILED_REPORT[@]}" "$DIR_HOST"|"$DIR)
		else
			LOG_MSG "[INFO]:-Located status report file"
			STATUS=`$TRUSTED_SHELL $DIR_HOST "$GREP -c \"Finished successfully\" $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_status_0_${DBID}_$TIMESTAMP_KEY"`
			if [ $STATUS -eq 1 ];then
				LOG_MSG "[INFO]:-Status report file indicates dump successful"
			else
				LOG_MSG "[WARN]:-Status report file indicates dump failure"
				LOG_MSG "[INFO]:-Status log file contents"
				LOG_MSG "[INFO]:-------------------------"
				$TRUSTED_SHELL $DIR_HOST "$CAT $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_status_0_${DBID}_$TIMESTAMP_KEY" >> $LOG_FILE
				LOG_MSG "[INFO]:-------------------------"
				ROLLBACK_DUMPS=1
				EXIT_STATUS=1
			fi
		fi
		#Checking for dump file
		EXISTS=`$TRUSTED_SHELL $DIR_HOST "if [ -f $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump_0_${DBID}_$TIMESTAMP_KEY*  ];then $ECHO 0;else $ECHO 1;fi"`
		if [ $EXISTS -eq 1 ];then
			LOG_MSG "[WARN]:-Could not locate dump file"
			EXIT_STATUS=1
			ROLLBACK_DUMPS=1
			FAILED_DUMP=(${FAILED_DUMP[@]}" "$DIR_HOST"|"$DIR)
		else
			LOG_MSG "[INFO]:-Located dump file"
		fi 
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

REPORT_FAILED_DUMPS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Missing status files list"
	LOG_MSG "[INFO]:--------------------------"
	for REP in ${FAILED_REPORT[@]}
	do
		LOG_MSG "[INFO]:-No status file for $REP"
	done
	LOG_MSG "[INFO]:--------------------------"
	LOG_MSG "[INFO]:-Missing dump files list"
	LOG_MSG "[INFO]:------------------------"
	for DFILE in ${FAILED_DUMP[@]}
	do
		LOG_MSG "[INFO]:-No dump file for $DFILE"
	done
	LOG_MSG "[INFO]:------------------------"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}	
DELETE_CURRENT_DUMP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	for DBID in ${DBID_LIST[@]}
	do
		CHK_DATA
		
		LOG_MSG "[INFO]:-Deleting dump files on $DIR_HOST segment dir $DIR"
		$TRUSTED_SHELL $DIR_HOST "$RM -f $DIR/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump*$TIMESTAMP_KEY*"
	done
	LOG_MSG "[INFO]:-Deleting Master segment dump files"
	$RM -f $MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR/$CUR_DATE/mpp_dump*$TIMESTAMP_KEY*
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DELETE_OLD_DUMPS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Get a list of all dbids
	DBID_LIST=(`$PSQL -p $MASTER_PORT $DEFAULTDB -R" " -A -t -c"select dbid from $MPP_PG_VIEW where content<>-1;"`)
	COUNT=0
	for DBID in ${DBID_LIST[@]}
	do
		CHK_DATA
		LOG_MSG "[INFO]:-Processing $DIR_HOST segment directory $DIR"
		DEL_DIR=`$TRUSTED_SHELL $DIR_HOST "$LS $DIR/$DUMP_SUB_DIR|$TR ' ' '\n'|$GREP -v $CUR_DATE|$SORT|$HEAD -1"`
		if [ x"" == x"$DEL_DIR" ];then
			LOG_MSG "[INFO]:-No previous old dump directories located $DIR_HOST in segment directory $DIR"
		else
			DEL_ARRAY[$COUNT]=$DEL_DIR
		fi
	done
			DEL_SET=`$ECHO ${DEL_ARRAY[@]}|$TR ' ' '\n'|$SORT|$HEAD -1`
	if [ x"" != x"$DEL_SET" ];then
		for DBID in ${DBID_LIST[@]}
		do
			CHK_DATA
			LOG_MSG "[INFO]:-Preparing to remove $DEL_SET on $DIR_HOST segment directory $DIR"
			$TRUSTED_SHELL $DIR_HOST "$RM -Rf $DIR/$DUMP_SUB_DIR/$DEL_SET"
			ERROR_CHK $? "delete $DEL_SET on $DIR_HOST segment directory $DIR" 1
		done
		LOG_MSG "[INFO]:-Processing Master segment instance"
		DEL_DIR=$MASTER_DATA_DIRECTORY/$DUMP_SUB_DIR/$DEL_SET
		LOG_MSG "[INFO]:-Preparing to remove $DEL_DIR on Master segment instance"	
		$RM -Rf $DEL_DIR
		ERROR_CHK $? "delete $DEL_DIR on Master segment instance" 1
	else
		LOG_MSG "[INFO]:-No old backup sets to remove"
		DEL_SET="None Located"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STATUS_REPORT () {
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	LOG_MSG "[INFO]:-Dump status report"
	LOG_MSG "[INFO]:--------------------------------------------"
	LOG_MSG "[INFO]:-Target database                    = $DUMP_DATABASE"
	LOG_MSG "[INFO]:-Dump sub directory                 = $CUR_DATE"
	if [ $CLEAR_DUMPS -eq 1 ];then
	LOG_MSG "[INFO]:-Clear old dump directories         = On"
	LOG_MSG "[INFO]:-Backup set deleted                 = $DEL_SET"
	else
	LOG_MSG "[INFO]:-Clear old dump directories         = Off"
	fi
	LOG_MSG "[INFO]:-Dump type                          = $TYPE"
	LOG_MSG "[INFO]:-Dump start time                    = $TIME_START"
	LOG_MSG "[INFO]:-Dump end time                      = $TIME_END"
	LOG_MSG "[INFO]:-Number of segments dumped          = ${#DBID_LIST[@]}"
	if [ $ROLLBACK_DUMPS ];then
	LOG_MSG "[WARN]:-Status                             = FAILED, Rollback Called"
	LOG_MSG "[INFO]:-See dump log file for errors"
	LOG_MSG "[WARN]:-Dump key                           = Not Applicable"
	else
	LOG_MSG "[INFO]:-Status                             = COMPLETED"
	LOG_MSG "[INFO]:-Dump key                           = $TIMESTAMP_KEY"
	fi
	if [ $COMPRESSION ];then
	LOG_MSG "[INFO]:-Dump file compression              = On"
	else
	LOG_MSG "[INFO]:-Dump file compression              = Off"
	fi
	if [ $EXIT_STATUS -ne 0 ];then
	LOG_MSG "[WARN]:-Exit code not zero, check log file"
	else
	LOG_MSG "[INFO]:-Exit code zero, no warnings generated"
	fi
	LOG_MSG "[INFO]:--------------------------------------------"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
		while getopts ":v'?'abiorqd:x:f:s:pmbcz" opt;do
			case $opt in
				v ) VERSION_INFO ;;
				'?' ) USAGE ;;
				d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
				r ) ROLLBACK=1 ;;
				b ) DISK_BYPASS=1 ;;
				i ) CHK_PARAM_BYPASS=1 ;;
				a ) unset INTERACTIVE ;;
				q ) unset VERBOSE ;;
				p ) DUMP_TYPE=1 ;;
				m ) DUMP_TYPE=2 ;;
				z ) unset COMPRESSION ;;
				f ) FREE_SPACE_PERCENT=$OPTARG ;;
				c ) CLEAR_DUMPS=1 ;;
				o ) LOG_MSG "[INFO]:-Start Main"
				    LOG_MSG "[INFO]:-Commencing clear old dump files only"
				    CHK_QD_DB_RUNNING
				    CHK_ON_PASSIVE_STANDBY
				    GET_MASTER_PORT $MASTER_DATA_DIRECTORY
				    DELETE_OLD_DUMPS
				    LOG_MSG "[INFO]:-End Main"
				    exit $EXIT_STATUS ;;
				s ) SCHEMA_NAME=$OPTARG ;;
				l ) LOG_DIR=$OPTARG ;;
				x ) DUMP_DATABASE=$OPTARG ;;
				* ) USAGE ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory"
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
OPTIONS_LIST="$*"
if [ x"" == x"$DUMP_DATABASE" ];then
	ERROR_EXIT "[FATAL]:-Must supply -x <database name to dump> " 2
fi
if [ $FREE_SPACE_PERCENT -gt 100 ] || [ $FREE_SPACE_PERCENT -lt 1 ];then
	ERROR_EXIT "[FATAL]:-Invalid value $FREE_SPACE_PERCENT % for  -f option" 2
fi
CHK_QD_DB_RUNNING
CHK_ON_PASSIVE_STANDBY
GET_MASTER_PORT $MASTER_DATA_DIRECTORY
if [ x"" == x"$CHK_PARAM_BYPASS" ];then
	CHK_PARAM
else
	DUMP_MIR_CHK
	LOG_MSG "[WARN]:-Parameter check phase bypassed, -i option supplied"
fi
if [ x"" == x"$DISK_BYPASS" ];then
	CHK_DUMP_SPACE
else
	LOG_MSG "[WARN]:-Disk space check bypassed, -b option supplied"
	GET_DUMP_LIST
fi
if [ $INTERACTIVE ]; then
	PROMPT_CONTINUE
else
	SET_DUMP_TYPE_TEXT
fi
PROCESS_DUMP
CHK_POST_DUMP
if [ $ROLLBACK_DUMPS ];then
	if [ $ROLL_BACK -eq 1 ];then
		LOG_MSG "[WARN]:-Dump request was incomplete, rolling back dump"
		DELETE_CURRENT_DUMP
		REPORT_FAILED_DUMPS
		EVENT_MAIL "Report from mppcrondump.sh on host $CUR_HOST [FAILED]" "Failed for database $DUMP_DATABASE with return code $EXIT_STATUS dump files rolled back. Options passed [$OPTIONS_LIST]"
		ERROR_EXIT "[FATAL]:-Dump incomplete, completed rollback" 2
	else
		LOG_MSG "[WARN]:-Dump request was incomplete, not rolling back dump as -n option supplied"
		REPORT_FAILED_DUMPS
		EVENT_MAIL "Report from mppcrondump.sh on host $CUR_HOST [FAILED]" "Failed for database $DUMP_DATABASE with return code $EXIT_STATUS dump files not rolled back. Options passed [$OPTIONS_LIST]"
		ERROR_EXIT "[FATAL]:-Dump incomplete, rollback not processed" 2
	fi
fi
if [ $CLEAR_DUMPS -eq 1 ];then
	DELETE_OLD_DUMPS
fi
STATUS_REPORT
EVENT_MAIL "Report from mppcrondump.sh on host $CUR_HOST" "Completed for database $DUMP_DATABASE with return code $EXIT_STATUS Options passed [$OPTIONS_LIST]"
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
