#!/bin/bash
#	Filename:-		gpdeletepart.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		July 2006
#	Release stat:-		Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Adds child tables from a parent table and
#				generates rules for data loading.
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
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
PROG_NAME=`$BASENAME $0`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
QD_NAME=`hostname`
EXIT_STATUS=0
RULE_GEN=0
INDEX_GEN=0
TMP_FILE=/tmp/${PROG_NAME}_${CUR_DATE}_TMP.$$
IDX_TMP_FILE=/tmp/${PROG_NAME}_${CUR_DATE}_TMP_IDX.$$
CHILD_COUNT_REC=0
ABORT=0
COPY_REC=0
PROCESS_COPY_REC=0
ARCHIVE=0
FORCE_DELETE=0
#******************************************************************************
# Functions
#******************************************************************************
#
USAGE () {
	if [ -f ${GPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
		$MORE ${GPDOCDIR}/$HELP_DOC_NAME
		exit 0
	else
		$ECHO	
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Deletes child tables that are outside of the rolling window (see -r option)"
		$ECHO "      Will delete child tables that are -r days/weeks/months/years old from current date"
		$ECHO "      Requires that the partitioning field is of type date or timestamp"
		$ECHO "      At this stage only supports date type range partitioning"
		$ECHO "      Assumes that current child tables, rules and indexes initially created "
		$ECHO "      via use of gpcreatepart.sh script, or were added via use of gpaddpart.sh"
		$ECHO "      The current database date is used as the start date from which the rolling"
		$ECHO "      window is calculated"
		$ECHO "      Note, that for hour based range partitioning, all tables for a 24hr period"
		$ECHO "      will be deleted, if they fall outside of the rolling window"
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
		$ECHO "      -a, don't ask to confirm generation process [default:- ask]"
		$ECHO "      -D, set log output to debug level, shows all function calls"
		$ECHO
		$ECHO "      Connection options:"
		$ECHO "      -d, <Master instance directory> name of alternative Master instance to run against [optional]"
		$ECHO
		$ECHO "      Child table deletion options:"
		$ECHO "      -t, <parent table name> name of parent table from which to delete child tables [mandatory]"
		$ECHO "      -r, <#> rolling window to keep based upon constraint type, for example if constraint"
		$ECHO "          type is m (months) -r 18 will keep a rolling window of 18 months"
		$ECHO "      -s, <archive table name> secure records to archive table prior to deletion of child table"
		$ECHO "          This cannot be the parent table. To create an archive table, run following in psql session"
		$ECHO "          psql=# create table <archive table name> (like <parent table name>);"
		$ECHO "      -f, force deletion if child tables are also parent tables"
		$ECHO "      -x, <Alternative database name>"
		$ECHO
		$ECHO "      Return codes:"
		$ECHO "      0 No problems encountered with requested operation"
		$ECHO "      1 Warning generated, but child deletion complete"
		$ECHO "      2 Fatal error, unable to complete child deletion request"
		$ECHO
		exit $EXIT_STATUS
	fi
}

UNKNOWN_OPTION () {
		$ECHO
		$ECHO "[ERROR]:-Unknown option $1"
		USAGE
}

PROMPT_CONTINUE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		SUFFIX_COUNT=${#SUFFIX_ARRAY[@]}
		LOG_MSG "[INFO]:-Partitioning deletion parameters:" 1
		LOG_MSG "[INFO]:----------------------------------" 1
		LOG_MSG "[INFO]:-Database                   = $QD_DBNAME" 1
		LOG_MSG "[INFO]:-Parent table               = $PARENT_TABLE_NAME" 1
		LOG_MSG "[INFO]:-Constraint type obtained   = $TYPE_NAME" 1
	 	LOG_MSG "[INFO]:-Current max rule date      = $MAX_DATE" 1
		LOG_MSG "[INFO]:-Current min rule date      = $MIN_DATE" 1
		case $TYPE_NAME in
			hour|daily) WINDOW_TEXT="Day(s)" ;;
			week) WINDOW_TEXT="Week(s)" ;;
			month) WINDOW_TEXT="Month(s)" ;;
			year) WINDOW_TEXT="Year(s)" ;;
		esac	
		LOG_MSG "[INFO]:-Window to keep on line     = $DELETION_WINDOW $WINDOW_TEXT" 1
		if [ ${#CHILD_IS_PARENT_ARRAY[@]} -ne 0 ];then
		LOG_MSG "[WARN]:-Nested partitioning        = Detected" 1
		LOG_MSG "[WARN]:-Child table delete mode    = Forced cascade" 1
		LOG_MSG "[INFO]:-Child tables that are parents" 1
		for P_NAME in ${CHILD_IS_PARENT_ARRAY[@]}
		do
		LOG_MSG "[INFO]:-$P_NAME is parent" 1
		done
		fi
		if [ $ARCHIVE -eq 0 ];then
		LOG_MSG "[INFO]:-Archive records            = Off" 1
		if [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ];then
			LOG_MSG "[INFO]:-----------------WARNING-------------------" 1
			LOG_MSG "[WARN]:-Child tables have records" 1
			LOG_MSG "[WARN]:-Records in child tables will be DELETED" 1
			LOG_MSG "[INFO]:-To save records abort this current session" 1
			LOG_MSG "[INFO]:-Then use -s <archive tablename> option" 1
			LOG_MSG "[INFO]:-------------------------------------------" 1
			EXIT_STATUS=1
		fi
		else
		LOG_MSG "[INFO]:-Archive records            = On" 1
		LOG_MSG "[INFO]:-Archive table              = $ARCHIVE_TABLE" 1
		fi
		LOG_MSG "[INFO]:-Existing child table count = $CHILD_NAME_ARRAY_COUNT" 1
		LOG_MSG "[INFO]:-Existing child rule count  = $CHILD_RULE_ARRAY_COUNT" 1
		LOG_MSG "[INFO]:-Existing child index count = $CHILD_INDEX_ARRAY_COUNT" 1
		LOG_MSG "[INFO]:-Delete child table count   = ${#CHILD_DELETE_ARRAY[@]}" 1
		LOG_MSG "[INFO]:-Delete child table name list" 1
		LOG_MSG "[INFO]:-----------------------------" 1
		for C_NAME in ${CHILD_DELETE_ARRAY[@]}
		do
		LOG_MSG "[INFO]:-$C_NAME" 1
		done
		$ECHO
		if [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ];then
			$ECHO "Continue with child deletion AND records contained in those child tables yY|nN >"
		else
			$ECHO "Continue with child deletion process yY|nN >"
		fi
		read REPLY
		if [ -z $REPLY ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
				exit 1
		fi
		if [ $REPLY != Y ] && [ $REPLY != y ]; then
				LOG_MSG "[WARN]:-User abort requested, Script Exits!" 1
				exit 1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}	

CHK_PARAM () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ x"" == x"$PARENT_TABLE_NAME" ];then
		ERROR_EXIT "[FATAL]:-Mandatory parameter -t <parent table name> not supplied" 2
	else
		if [ `$ECHO $PARENT_TABLE_NAME|$GREP -c "\."` -eq 1 ];then
			SCHEMA_NAME=`$ECHO $PARENT_TABLE_NAME|$CUT -d"." -f1`
			TABLE_NAME=`$ECHO $PARENT_TABLE_NAME|$CUT -d"." -f2`
		else
			ERROR_EXIT "[FATAL]:-Table name supplied via -t not prefixed with schema name" 2
		fi
	fi	
	if [ x"" != x"$ARCHIVE_TABLE" ];then
		LOG_MSG "[INFO]:-Archive table $ARCHIVE_TABLE supplied, will secure records prior to deletion" 1
		ARCHIVE=1
		if [ `$ECHO $ARCHIVE_TABLE|$GREP -c "\."` -eq 1 ];then
			ARCHIVE_SCHEMA=`$ECHO $ARCHIVE_TABLE|$CUT -d"." -f1`
			ARCHIVE_TABLE_NAME=`$ECHO $ARCHIVE_TABLE|$CUT -d"." -f2`
		else
			ERROR_EXIT "[FATAL]:-Table name supplied via -s not prefixed with schema name" 2
		fi
		if [ $PARENT_TABLE_NAME == $ARCHIVE_TABLE ];then
			LOG_MSG "[FATAL]:-Cannot secure records into the parent table" 1
			LOG_MSG "[INFO]:-Create a new table like the parent table" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		fi
	fi	
	if [ x"" == x"$DELETION_WINDOW" ];then
		ERROR_EXIT "[FATAL]:-Mandatory parameter -r <#> not supplied" 2
	else
		if [ $DELETION_WINDOW -lt 0 ] || [ `$ECHO $DELETION_WINDOW|$EGREP -c "[A-Z]|[a-z]"` -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Problem with value supplied with -r option." 2
		fi
	fi
	if [ x"" != x"$ALT_DB_NAME" ];then
		$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $ALT_DB_NAME -l > /dev/null 2>&1
		ERROR_CHK $? "connect to $ALT_DB_NAME database" 2
		QD_DBNAME=$ALT_DB_NAME
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

VALIDATE_PROCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check that the table exists
	CHK_EXISTS_TABLE $PARENT_TABLE_NAME $MASTER_PORT $QD_DBNAME
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Parent table $PARENT_TABLE_NAME does not exist in $QD_DBNAME database" 2
	fi
	if [ x"" != x"$ARCHIVE_TABLE" ];then
		CHK_EXISTS_TABLE $ARCHIVE_TABLE $MASTER_PORT $QD_DBNAME
		if [ $EXISTS -ne 0 ];then
			ERROR_EXIT "[FATAL]:-Archive table $ARCHIVE_TABLE does not exist." 2
		fi
	fi
	#Check to see if table has any child tables already
	HAS_CHILD_TABLES=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select relhassubclass from pg_class where relname = '$TABLE_NAME' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
	ERROR_CHK $? "obtain sub class information" 2
	if [ $HAS_CHILD_TABLES != 't' ];then
		ERROR_EXIT "[FATAL]:-Table $PARENT_TABLE_NAME does not have any child tables defined." 2
	fi
	#Now get partition type
	CHILD_NAME=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -R" " -A -t $QD_DBNAME -c"select relname from pg_class where oid in (select inhrelid from pg_inherits where inhparent in (select oid from pg_class where relname='$TABLE_NAME' and relnamespace in (select oid from pg_namespace where nspname='${SCHEMA_NAME}'))) limit 1;"`
        ERROR_CHK $? "obtain child table name" 2
        TYPE_NAME=`$ECHO $CHILD_NAME|$AWK -F"_" '{print $NF}'`
	case $TYPE_NAME in
		hour|daily|week|month|year )
			LOG_MSG "[INFO]:-Partition type is $TYPE_NAME" 1
			;;
		range|list )
			LOG_MSG "[INFO]:-Partition type is $TYPE_NAME" 1
			ERROR_EXIT "[FATAL]:-Unable to process this partitioning type at present" 2
			;;
		* )
			ERROR_EXIT "[FATAL]:-Partition type $TYPE_NAME is unknown" 2
			;;
	esac
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GATHER_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Obtaining child table list"
	CHILD_NAME_ARRAY=(`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -R" " -A -t $QD_DBNAME -c"select relname from pg_class where oid in (select inhrelid from pg_inherits where inhparent in (select oid from pg_class where relname='$TABLE_NAME' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME'))) order by relname;"`)
	ERROR_CHK $? "child table list" 2
	CHILD_NAME_ARRAY_COUNT=${#CHILD_NAME_ARRAY[@]}
	LOG_MSG "[INFO]:-Table $TABLE_NAME has $CHILD_NAME_ARRAY_COUNT child tables" 1
	LOG_MSG "[INFO]:-Checking for rules on child tables" 1
	CHILD_RULE_ARRAY_COUNT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -R" " -A -t $QD_DBNAME -c"select count(*) from pg_rules where tablename='$TABLE_NAME' and schemaname='${SCHEMA_NAME}';"`
	ERROR_CHK $? "obtain count of number of rules" 2
	if [ $CHILD_RULE_ARRAY_COUNT -ne 0 ];then
		LOG_MSG "[INFO]:-Table $TABLE_NAME has $CHILD_RULE_ARRAY_COUNT rules" 1
	else
		LOG_MSG "[WARN]:-Table $TABLE_NAME has $CHILD_RULE_ARRAY_COUNT rules" 1
		EXIT_STATUS=1
	fi
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_INDEX=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -R" " -A -t $QD_DBNAME -c"select indexname from pg_indexes where tablename='$C_NAME' and schemaname='$SCHEMA_NAME';"`
		ERROR_CHK $? "obtain child index list for $C_NAME" 1
		CHILD_INDEX_ARRAY=(${CHILD_INDEX_ARRAY[@]} $CHILD_INDEX)
		#Now check to see if child table is a parent
		CHILD_TAB_NAME=`$ECHO $C_NAME|$CUT -d"." -f2`
		CHILD_IS_PARENT=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select relhassubclass from pg_class where relname = '$CHILD_TAB_NAME' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
		ERROR_CHK $? "obtain has sub class for $C_NAME" 1
		if [ $CHILD_IS_PARENT == 't' ];then
			LOG_MSG "[WARN]:-Child table $C_NAME is also a parent" 1
			EXIT_STATUS=1
			CHILD_IS_PARENT_ARRAY=(${CHILD_IS_PARENT_ARRAY[@]} $C_NAME)
		fi
	done
	if [ ${#CHILD_IS_PARENT_ARRAY[@]} -ne 0 ] && [ $FORCE_DELETE -ne 1 ];then
		ERROR_EXIT "[FATAL]:-Nested partitioning detected and -f option not supplied" 2
	fi
	if [ ${#CHILD_IS_PARENT_ARRAY[@]} -ne 0 ] && [ $FORCE_DELETE -eq 1 ];then
		LOG_MSG "[INFO]:-Nested partitioning detected will cascade delete" 1
	fi
	if [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ] && [ x"" == x"$ARCHIVE_TABLE" ];then
		LOG_MSG "[WARN]:-${#CHILD_WITH_REC_ARRAY[@]} child tables contain records and no archive table name supplied" 1
		LOG_MSG "[WARN]:-Data will be lost following child table deletion" 1
		EXIT_STATUS=1
	else
		LOG_MSG "[INFO]:-Records detected in child tables, will secure to $ARCHIVE_TABLE" 1
	fi
	CHILD_INDEX_ARRAY_COUNT=${#CHILD_INDEX_ARRAY[@]}
	#Now get the min and max date ranges from the rules table if number of rules <> 0
	if [ $CHILD_RULE_ARRAY_COUNT -ne 0 ] && [ $CHILD_RULE_ARRAY_COUNT -eq $CHILD_NAME_ARRAY_COUNT ] ;then
		MIN_DATE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -d $QD_DBNAME -A -t -c"select definition from pg_rules where tablename='$TABLE_NAME' and schemaname='$SCHEMA_NAME';"|$AWK -F"'" '{print $2}'|$GREP "-"|$SORT|$HEAD -1|$AWK '{print $1}'`
		ERROR_CHK $? "obtain start partition date " 2
		case $TYPE_NAME in
			hour|week|month|year )
			MAX_DATE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -d $QD_DBNAME -A -t -c"select definition from pg_rules where tablename='$TABLE_NAME' and schemaname='$SCHEMA_NAME';"|$AWK -F"'" '{print $4}'|$GREP "-"|$SORT|$TAIL -1|$AWK '{print $1}'`
			ERROR_CHK $? "obtain end partition date " 2
			;;
			daily )
			MAX_DATE=`$EXPORT_LIB_PATH;env PGOPTIONS="-c gp_session_role=utility" $PSQL -d $QD_DBNAME -A -t -c"select definition from pg_rules where tablename='$TABLE_NAME' and schemaname='$SCHEMA_NAME';"|$AWK -F"'" '{print $2}'|$GREP "-"|$SORT|$TAIL -1|$AWK '{print $1}'`
			ERROR_CHK $? "obtain end partition date " 2
			;;
		esac
		LOG_MSG "[INFO]:-Partition minimum date   = $MIN_DATE" 1
		LOG_MSG "[INFO]:-Partition maximum date   = $MAX_DATE" 1
	else
		if [ $CHILD_RULE_ARRAY_COUNT -eq 0 ];then
			LOG_MSG "[WARN]:-Unable to process rule table as no rules defined for partitioning" 1
		else
			LOG_MSG "[WARN]:-Unable to process rule table as #rules <> #child tables" 1
		fi
		EXIT_STATUS=1
		((LAST=$CHILD_NAME_ARRAY_COUNT-1))
		case $TYPE_NAME in
			hour )
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[0]}|$AWK -F"_" '{print $(NF-4)$(NF-3)$(NF-2)}'`
				MAX_DATE=`$ECHO ${CHILD_NAME_ARRAY[${LAST}]}|$AWK -F"_" '{print $(NF-4)$(NF-3)$(NF-2)}'`
				;;
			daily )
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[0]}|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'`
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[${LAST}]}|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'`			
				;;
			week )
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[0]}|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'|$TR -d 'w'`
				MAX_DATE=`$ECHO ${CHILD_NAME_ARRAY[${LAST}]}|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'|$TR -d 'w'`
				;;
			month )
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[0]}|$AWK -F"_" '{print $(NF-2)$(NF-1)}'`	
				MAX_DATE=`$ECHO ${CHILD_NAME_ARRAY[${LAST}]}|$AWK -F"_" '{print $(NF-2)$(NF-1)}'`	
				;;
			year )
				MIN_DATE=`$ECHO ${CHILD_NAME_ARRAY[0]}|$AWK -F"_" '{print $(NF-1)}'`
				MAX_DATE=`$ECHO ${CHILD_NAME_ARRAY[${LAST}]}|$AWK -F"_" '{print $(NF-1)}'`
				;;
		esac
		LOG_MSG "[INFO]:-Min date from child name array is $MIN_DATE" 1
		LOG_MSG "[INFO]:-Max date from child name array is $MAX_DATE" 1
	fi	
	PARENT_TABLE_COUNT=`$EXPORT_LIB_PATH; $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select count(*) from only $PARENT_TABLE_NAME;"`
	ERROR_CHK $? "obtain parent record count" 2
	LOG_MSG "[INFO]:-Parent table $PARENT_TABLE_NAME has $PARENT_TABLE_COUNT records" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

COMPUTE_WINDOW () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Calculate rolling window period
	CURRENT_DB_DATE=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select date(now());"`
	ERROR_CHK $? "obtain current database date" 2
	LOG_MSG "[INFO]:-Current database date is $CURRENT_DB_DATE" 1
	case $TYPE_NAME in
		hour)
			PROCESS_HOUR ;;
		daily) 
			PROCESS_DAILY ;;
		week) 
			PROCESS_WEEKLY ;;
		month) 
			PROCESS_MONTHLY ;;
		year)
			PROCESS_YEARLY ;;
	esac
	if [ ${#CHILD_DELETE_ARRAY[@]} -ne 0 ];then
		for C_NAME in ${CHILD_DELETE_ARRAY[@]}
		do
			CHILD_REC_COUNT=`$EXPORT_LIB_PATH; $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select count(*) from ${SCHEMA_NAME}.$C_NAME ;"`
			if [ $CHILD_REC_COUNT -ne 0 ];then
				CHILD_WITH_REC_ARRAY=(${CHILD_WITH_REC_ARRAY[@]} ${SCHEMA_NAME}.$C_NAME)
				LOG_MSG "[INFO]:-Child table $C_NAME has record count = $CHILD_REC_COUNT" 1
			fi
		done
		if [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ] && [ x"" == x"$ARCHIVE_TABLE" ];then
			LOG_MSG "[WARN]:-${#CHILD_WITH_REC_ARRAY[@]} child tables contain records and no archive table name supplied" 1
                	LOG_MSG "[WARN]:-Data will be lost following child table deletion" 1
                	EXIT_STATUS=1
        	else
                	LOG_MSG "[INFO]:-Records detected in child tables, will secure to $ARCHIVE_TABLE" 1
        	fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_HOUR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	WINDOW_START_DATE=`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select date('$CURRENT_DB_DATE') - $DELETION_WINDOW;"`
	ERROR_CHK $? "obtain rolling date window" 2
	WINDOW_START_NUM=`$ECHO $WINDOW_START_DATE|$TR -d '-'`
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_NUM=`$ECHO $C_NAME|$AWK -F"_" '{print $(NF-4)$(NF-3)$(NF-2)}'`
		if [ $CHILD_NUM -le $WINDOW_START_NUM ];then
			CHILD_DELETE_ARRAY=(${CHILD_DELETE_ARRAY[@]} $C_NAME)
			LOG_MSG "[INFO]:-Marked $C_NAME table for deletion" 1
		fi
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_DAILY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	#Check to see if there are any child tables that are older than rolling delete window
	WINDOW_START_DATE=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select date('$CURRENT_DB_DATE') - $DELETION_WINDOW;"`
	ERROR_CHK $? "obtain rolling date window" 2
	WINDOW_START_NUM=`$ECHO $WINDOW_START_DATE|$TR -d '-'`
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_NUM=`$ECHO $C_NAME|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'`
		if [ $CHILD_NUM -le $WINDOW_START_NUM ];then
			CHILD_DELETE_ARRAY=(${CHILD_DELETE_ARRAY[@]} $C_NAME)
			LOG_MSG "[INFO]:-Marked $C_NAME table for deletion" 1
		fi
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_WEEKLY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	((DAYS_BACK=$DELETION_WINDOW*7))
	WINDOW_START_DATE=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select date('$CURRENT_DB_DATE') - $DAYS_BACK ;"`
	ERROR_CHK $? "obtain start delete date" 2
	WINDOW_START_WEEK=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select LPAD(extract (week from date'$WINDOW_START_DATE'),2,'0');"`
	ERROR_CHK $? "obtain start delete week" 2
	WINDOW_DEL_SUFFIX=`$ECHO $WINDOW_START_DATE|$CUT -d'-' -f1-2|$TR -d '-'`$WINDOW_START_WEEK
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_NUM=`$ECHO $C_NAME|$AWK -F"_" '{print $(NF-3)$(NF-2)$(NF-1)}'|$TR -d 'w'`
		if [ $CHILD_NUM -le $WINDOW_DEL_SUFFIX ];then
			CHILD_DELETE_ARRAY=(${CHILD_DELETE_ARRAY[@]} $C_NAME)
		fi
	done	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_MONTHLY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CURRENT_MONTH=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select LPAD(extract (month from date('now()')),2, '0');"`
	ERROR_CHK $? "obtain current month" 2
	LOG_MSG "[INFO]:-Current month is $CURRENT_MONTH" 1
	CURRENT_YEAR=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select extract (year from date('now()'));"`	
	ERROR_CHK $? "obtain current year" 2
	LOG_MSG "[INFO]:-Current year is $CURRENT_YEAR" 1
	COUNT=1
	#Now get the year going back $DELETION_WINDOW months
	DELETE_YEAR=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select extract (year from (select now() - interval '$DELETION_WINDOW months'));"`
	ERROR_CHK $? "obtain delete start year" 2
	DELETE_MONTH=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select LPAD(extract (month from (select now() - interval '$DELETION_WINDOW months')),2, '0');"`
	ERROR_CHK $? "obtain delete start month" 2
	WINDOW_DEL_SUFFIX=$DELETE_YEAR$DELETE_MONTH
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_NUM=`$ECHO $C_NAME|$AWK -F"_" '{print $(NF-2)$(NF-1)}'`	
		if [ $CHILD_NUM -le $WINDOW_DEL_SUFFIX ];then
			CHILD_DELETE_ARRAY=(${CHILD_DELETE_ARRAY[@]} $C_NAME)
		fi
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_YEARLY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	CURRENT_YEAR=`$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select LPAD(extract (year from date('now()')),4, '0');"`
	ERROR_CHK $? "obtain current year" 2
	LOG_MSG "[INFO]:-Current year is $CURRENT_YEAR" 1
	COUNT=1
	((DELETE_YEAR=$CURRENT_YEAR-$DELETION_WINDOW))
	WINDOW_DEL_SUFFIX=$DELETE_YEAR
	for C_NAME in ${CHILD_NAME_ARRAY[@]}
	do
		CHILD_NUM=`$ECHO $C_NAME|$AWK -F"_" '{print $(NF-1)}'`
		if [ $CHILD_NUM -le $WINDOW_DEL_SUFFIX ];then
			CHILD_DELETE_ARRAY=(${CHILD_DELETE_ARRAY[@]} $C_NAME)
		fi
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PROCESS_DELETION () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $ARCHIVE -eq 1 ] && [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ];then
			SECURE_RECORDS
	fi
	for C_NAME in ${CHILD_DELETE_ARRAY[@]}
	do
		LOG_MSG "[INFO]:-Starting delete cascade of $C_NAME"
		$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"drop table ${SCHEMA_NAME}.$C_NAME cascade;"	
		ERROR_CHK $? "delete of child table $C_NAME" 1	
	done
	if [ $ARCHIVE -eq 1 ] && [ ${#CHILD_WITH_REC_ARRAY[@]} -ne 0 ];then
		LOG_MSG "[INFO]:-Running analyze on $ARCHIVE_TABLE" 1
		$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"analyze $ARCHIVE_TABLE;"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SECURE_RECORDS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Securing records to archive table $ARCHIVE_TABLE" 1
	for SAVE_NAME in ${CHILD_WITH_REC_ARRAY[@]}
	do
		LOG_MSG "[INFO]:-Securing records from $SAVE_NAME to $ARCHIVE_TABLE" 1
		$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"insert into $ARCHIVE_TABLE (select * from $SAVE_NAME);"
	ERROR_CHK $? "secure ${SCHEMA_NAME}.$SAVE_NAME records to $ARCHIVE_TABLE" 2
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************

# Main Section
#******************************************************************************
trap 'ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
if [ $# -ne 0 ]; then
		while getopts ":vt:'?'aql:d:x:r:s:f!D" opt;do
				case $opt in
						v ) VERSION_INFO ;;
						t ) PARENT_TABLE_NAME=`$ECHO $OPTARG|$TR '[A-Z]' '[a-z]'` ;;
						'?' ) USAGE ;;
						! ) SCRIPT_USAGE=1
						    USAGE ;;
						d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
						a ) unset INTERACTIVE ;;
						q ) unset VERBOSE ;;
						l ) LOG_DIR=$OPTARG ;;
						x ) ALT_DB_NAME=$OPTARG ;;
						r ) DELETION_WINDOW=$OPTARG ;;
						s ) ARCHIVE_TABLE=`$ECHO $OPTARG|$TR '[A-Z]' '[a-z]'` ;;
						f ) FORCE_DELETE=1 ;;
						D ) DEBUG_LEVEL=1 ;;
						* ) USAGE ;;
				esac
		done	
fi
if [ x"" != x"$LOG_DIR" ];then
	CHK_DIR $LOG_DIR
	if [ $EXISTS -eq 1 ]; then
		LOG_MSG "[WARN]:-Log directory $LOG_DIR does not exist, using default log directory" 1
	else
		LOG_FILE=$LOG_DIR/${PROG_NAME}_${CUR_DATE}.log
	fi
fi
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ x"" != x"$GPHOME" ];then
	APP_PATH=${GPHOME}/bin
else
	APP_PATH=${MPPHOME}/bin
fi
#Check to ensure mandatory parameters have been supplied
CHK_ON_PASSIVE_STANDBY
GET_QD_DB_NAME
CHK_PARAM
VALIDATE_PROCESS
GATHER_INFO
COMPUTE_WINDOW
if [ ${#CHILD_DELETE_ARRAY[@]} -eq 0 ];then
	LOG_MSG "[INFO]:-No child tables marked for deletion" 1
else
	if [ x"" != x"$INTERACTIVE" ];then
		PROMPT_CONTINUE
	fi
	PROCESS_DELETION
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
