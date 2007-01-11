#!/bin/bash
#	Filename:-		mppskew.bsh
#	Version:-		$Revision$	
#	Updated:-		$Date$
#	Status:-		Released
#	Author:-		G L Coombe
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
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
# Script checks to ensure that table entered as parameter exists.
# If table name exists calls Get_Node_Count function which does the following;
# 1. Obtains port number, host and database name for all nodes in the instance
# 2. Issues a non-blocking select count(*) for table to each node
# 3. Waits for a response from all nodes in the instance
# 4. Displays table of data obtained
# 5. Computes maximum and minimum row counts and variance
# 6. Displays calculated output  
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
TMPFILE=/tmp/mppskew.tmp.$$
NODELIST=/tmp/nodelist.tmp.$$
PROG_NAME=`$BASENAME $0`
PROG_NAME=`$BASENAME $0`
HELP_DOC_NAME=`$ECHO $PROG_NAME|$AWK -F'.' '{print $1}'`_help
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
DISTRIB=0
SEG_TYPE=0
DO_CHILD=0
RESPONSE_TIME=0
DEBUG_LEVEL=0
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
if [ -f ${MPPDOCDIR}/$HELP_DOC_NAME ] && [ x"" == x"$SCRIPT_USAGE" ];then
	$MORE ${MPPDOCDIR}/$HELP_DOC_NAME
	exit 0
else
	GET_QD_DB_NAME >/dev/null 2>&1
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
        $ECHO "      Reports the data skew for a table within an MPP database."
	$ECHO "      Displays the segments with the most and least number of records,"
	$ECHO "      calculates the variance of records between these two segments,"
	$ECHO "      and reports the elapsed response time for the segment host."
        $ECHO
        $ECHO "      Usage:"
        $ECHO "      $0 [OPTIONS]"
        $ECHO
        $ECHO "      General options:"
        $ECHO "      -?, display this help message & exit"
        $ECHO "      -v, display version information & exit"
	$ECHO
	$ECHO "      Connection options:"
        $ECHO "      -d, <master instance directory> name of alternative master instance to connect [optional]"
	$ECHO
	$ECHO "      Logging options:"
	$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
	$ECHO "      -q, quiet mode, only useful where -f option also used"
	$ECHO "      -D, set log output to debug level, shows all function calls"
	$ECHO
	$ECHO "      Skew options:"
	$ECHO "      -t, <table name> [mandatory]"
	$ECHO "      -a, <database name> alternative database name [default ${QD_DBNAME}]"
	$ECHO "      -f, display full results output [default summary]"
	$ECHO "      -c, display information for table, do not compute skew"
	$ECHO "      -m, use mirror segment hosts for skew analysis [default primary segments]"
	$ECHO "      -h, display skew of child tables if -t <table name> is a partitioned table"
	$ECHO "      -r, display segment instance response times"
        $ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
fi
}

GET_DISTRIB_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_TABLE_TYPE $TARGETTBL $MASTER_PORT $QD_DBNAME
	PARENT_TAB_TYPE=$TAB_TYPE
	PARENT_TAB_PARENT_NAME=$TAB_PARENT_NAME
	if [ $DISTRIB_COLS != "0" ];then
		LOG_MSG "[INFO]:-Obtaining OID for $TABLENAME on Master instance"
		TABLE_OID=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select oid from pg_class where relname='$TABLENAME' and relkind='r' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
		if [ $? -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Failed obtain table oid value" 2;fi
	TOTAL_COLS=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select max(attnum) from pg_attribute where attrelid=${TABLE_OID};"`
	if [ $? -ne 0 ];then
	ERROR_EXIT "[FATAL]:-Failed to obtain table column count" 2;fi
	LOG_MSG "[INFO]:-------------------------------------------" 1
	LOG_MSG "[INFO]:-Table name              = $TARGETTBL" 1
	COLNUM=0
	for ATT_NUM in `echo $DISTRIB_COLS|$TR -d '{'|$TR -d '}'|$TR ',' ' '`
	do
		DIS_COL_NAME=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select attname from pg_attribute where attrelid=${TABLE_OID} and attnum=${ATT_NUM};"`
		((COLNUM=$COLNUM+1))
#	LOG_MSG "[INFO]:-Distrib col  = $COLNUM at DDL position $ATT_NUM is named $DIS_COL_NAME"
	LOG_MSG "[INFO]:-Distrib column count    = $COLNUM" 1
	LOG_MSG "[INFO]:-Distrib column name     = $DIS_COL_NAME" 1
	LOG_MSG "[INFO]:-Distrib column position = $ATT_NUM" 1
	done
	LOG_MSG "[INFO]:-Total columns           = $TOTAL_COLS" 1
	LOG_MSG "[INFO]:-Table is type           = $TAB_TYPE_TXT" 1
	if [ $PARTITIONED == 't' ];then
		PARENT_TABLE_COUNT=`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select count(*) from only $TARGETTBL;"`
		LOG_MSG "[INFO]:-Physical record count   = $PARENT_TABLE_COUNT"	 1
		LOG_MSG "[INFO]:-Table type              = Partitioned" 1
		GET_CHILD_NAMES
		LOG_MSG "[INFO]:-------------------------------------------" 1
		LOG_MSG "[INFO]:-Child table details" 1
		LOG_MSG "[INFO]:-------------------------------------------" 1
		for C_NAME in ${CHILD_NAME_ARRAY[@]}
		do
			GET_DISTRIB_COLS ${SCHEMA_NAME}.$C_NAME $MASTER_PORT $QD_DBNAME
			GET_TABLE_TYPE ${SCHEMA_NAME}.$C_NAME $MASTER_PORT $QD_DBNAME
			LOG_MSG "[INFO]:-$C_NAME distribution columns=$DISTRIB_COLS" 1
		done	
		LOG_MSG "[INFO]:-------------------------------------------" 1
		if [ $PARENT_TAB_TYPE -eq 1 ];then
			LOG_MSG "[INFO]:-Table $TABLENAME is also a child table" 1
			LOG_MSG "[INFO]:-Parent table name      =  $PARENT_TAB_PARENT_NAME" 1
		fi
	fi
	else
		LOG_MSG "[INFO]:-Distribution columns are of type special" 1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_CHILD_NAMES () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		PARENT_TABLE_RELFILENODE=`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select oid from pg_class where relname='$TABLENAME' and relkind='r' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
		CHILD_NAME_ARRAY=(`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select relname from pg_class where relkind='r' and oid in (select inhrelid from pg_inherits where inhparent=$PARENT_TABLE_RELFILENODE);"|$TR '\n' ' '`)
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_NODE_COUNT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ `$ECHO $TARGETTBL|$GREP -c "\."` -eq 1 ];then
		SCHEMA_NAME=`$ECHO $TARGETTBL|$CUT -d. -f1`
		T_NAME=`$ECHO $TARGETTBL|$CUT -d. -f2`
	fi
	SEGCOUNT=0
	DIS_COL_ARRAY=(`$PSQL -p $MASTER_PORT  $QD_DBNAME -R" " -A -t -c "select  a.attname from pg_class c,  mpp_distribution_policy d, pg_attribute a where c.oid = d.localoid and a.attrelid = d.localoid and  a.attnum = ANY ( d.attrnums ) and c.relname='$T_NAME' and c.relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`)
	NUM_SEGMENTS=`$PSQL -p $MASTER_PORT  $QD_DBNAME -A -t -c "select count(*) from $CONFIG_TABLE where content<>-1 and isprimary='t';"`
	ERROR_CHK $? "obtain number of segment instances" 2
	if [ $NUM_SEGMENTS = 1 ];then
		ERROR_EXIT "[FATAL]:-Only one node in instance, skew not applicable" 2;fi 
		if [ $SEG_TYPE -eq 0 ];then
		  $PSQL -p $MASTER_PORT $QD_DBNAME -t -A -F"," -c"select hostname, port, datadir from $CONFIG_TABLE where content <> -1 and isprimary='t' ;" > $NODELIST
		  ERROR_CHK $? "obtain list of active segments" 2
		else
		  $PSQL -p $MASTER_PORT $QD_DBNAME -t -A -F"," -c"select hostname, port, datadir from $CONFIG_TABLE where content <> -1 and isprimary='f' ;" > $NODELIST
		  ERROR_CHK $? "obtain list of mirror segments" 2
		fi

	LOG_MSG "[INFO]:-Sending request to segment instance databases.." 1
	LOG_MSG "[INFO]:-Please wait.." 1
	if [ `$ECHO $TARGETTBL|$GREP -c "\."` -eq 0 ];then
		TARGETTBL="${SCHEMA_NAME}.$TARGETTBL"
	fi
	SEGCOUNT=0
	for NodeInfo in `$CAT $NODELIST`
	do
	SEG_HOST=`$ECHO $NodeInfo|$CUT -d, -f1`
	SEG_PORT=`$ECHO $NodeInfo|$CUT -d, -f2`
	SEG_DIR=`$ECHO $NodeInfo|$CUT -d, -f3`
	SEG_NAME=`$BASENAME $SEG_DIR`
	ACCESS_TEST=`$TRUSTED_SHELL $SEG_HOST "${EXPORT_LIB_PATH};env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT $QD_DBNAME -A -t -c\"select version();\""`
	if [ $? -ne 0 ];then
		ERROR_EXIT "Failed on test access to $SEG_PORT $QD_DBNAME on $SEG_HOST" 2
	fi
	$TRUSTED_SHELL -n $SEG_HOST "${EXPORT_LIB_PATH}; time env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT $QD_DBNAME -A -t -c'select count(*),'\''$SEG_PORT'\'','\''$SEG_NAME'\'','\''$SEG_HOST'\'' from $TARGETTBL;'" 2>&1|$HEAD -3|$TR -s '\n'|$TR ' ' '\n'|$TR -d '.'|$TR -cs '[:alnum:]' '[\n*]'|$GREP -v real|$TR  '\n' '|'|$AWK '{print $1}' >> $TMPFILE &
	if [ $? -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Failed to send request to $SEG_HOST port $SEG_PORT" 2
	fi
	let SEGCOUNT=$SEGCOUNT+1
	done
	LOG_MSG "[INFO]:-Requests sent.." 1
#Now wait until the temporary file contains $SegCount lines
	LOG_MSG  "[INFO]:-Waiting for results" 1
	while [ $SEGCOUNT -ne `$WC -l $TMPFILE|$AWK '{print $1}'` ]
	do
		$NOLINE_ECHO ".\c"
		$SLEEP 1
	done
	$ECHO
	REC_COUNT=0
	for RECS in `cat $TMPFILE|$CUT -d'|' -f1`
	do
		((REC_COUNT=$REC_COUNT+$RECS))
	done
	MinSkew=10000000000000
	MaxSkew=-1
	while read DATA
	do
		Port=`$ECHO $DATA|$TAIL -1|$CUT -d'|' -f2`
		Skew=`$ECHO $DATA|$TAIL -1|$CUT -d'|' -f1`
		Seg=`$ECHO $DATA|$TAIL -1|$CUT -d'|' -f3`
		Host=`$ECHO $DATA|$TAIL -1|$CUT -d'|' -f4`
		if [ $Skew -gt $MaxSkew ];then
			MaxSkew=$Skew
			MaxSeg=$Seg
			MaxHost=$Host
			MaxPort=$Port
		fi
		if [ $Skew -lt $MinSkew ];then
			MinSkew=$Skew
			MinSeg=$Seg
			MinHost=$Host
			MinPort=$Port
		fi
	done < $TMPFILE
	let RecDiff=$MaxSkew-$MinSkew
	if [ $RESPONSE_TIME -eq 1 ];then
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-Response results for $TYPE_TEXT" 1
	while read LINE
	do
		Host=`$ECHO $LINE|$CUT -d '|' -f4`
		Time=`$ECHO $LINE|$CUT -d '|' -f5`
		Seg=`$ECHO $LINE|$CUT -d '|' -f3`
		Port=`$ECHO $LINE|$CUT -d '|' -f2`
		LOG_MSG "[INFO]:-Host $Host segment $Seg port $Port responded in time $Time" 1
	done < $TMPFILE
	fi
	if [ $FULL_DISPLAY -eq 1 ] || [ x"" = x"$VERBOSE" ];then
		$ECHO "Skew for table $TARGETTBL" | $TEE -a $LOG_FILE
		$CAT $TMPFILE|$SORT | $TEE -a $LOG_FILE
	else
		$CAT $TMPFILE|$SORT >> $LOG_FILE
	fi
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-Results for Table $TARGETTBL in database $QD_DBNAME" 1
	if [ $PARTITIONED == 't' ];then
	LOG_MSG "[INFO]:-Table type                           = Partitioned and $TAB_TYPE_TXT"
		PARENT_TABLE_COUNT=`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select count(*) from only $TARGETTBL;"`
		LOG_MSG "[INFO]:-Parent table physically contains $PARENT_TABLE_COUNT records"	 1
	else
	LOG_MSG "[INFO]:-Table type                           = Un-Partitioned and $TAB_TYPE_TXT" 1
	fi
	if [ $TAB_TYPE -eq 1 ];then
	LOG_MSG "[INFO]:-Parent table list                    = ${TAB_PARENT_NAME[@]}" 1
	fi
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-DDL number of Distribution Column(s) = $DISTRIB_COLS" 1
	LOG_MSG "[INFO]:-Distribution Column Name(s)          = `$ECHO ${DIS_COL_ARRAY[@]}`" 1
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-Total records                        = $REC_COUNT" 1
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-Distribution for $TYPE_TEXT" 1
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	LOG_MSG "[INFO]:-Skew result" 1
	LOG_MSG "[INFO]:-Maximum Record Count                 = $MaxSkew" 1
	LOG_MSG "[INFO]:-Segment instance hostname            = $MaxHost" 1
	LOG_MSG "[INFO]:-Segment instance name                = $MaxSeg" 1
	LOG_MSG "[INFO]:-Minimum Record Count                 = $MinSkew" 1
	LOG_MSG "[INFO]:-Segment instance hostname            = $MinHost" 1
	LOG_MSG "[INFO]:-Segment instance name                = $MinSeg" 1
	LOG_MSG "[INFO]:-Record count variance                = $RecDiff" 1
	LOG_MSG "[INFO]-------------------------------------------------------------" 1
	rm -f $TMPFILE
	rm -f $NODELIST
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap '$RM -f $TMPFILE $NODELIST;ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
CHK_DB_QD_SET
if [ $# -ne 0 ];then
        while getopts ":a:vmcqft:'?'d:l:!hrD" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                t ) TARGETTBL=`$ECHO $OPTARG|$TR '[A-Z]' '[a-z]'` ;;
                '?' ) USAGE ;;
		! ) SCRIPT_USAGE=1
		    USAGE ;;
                d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
		m ) SEG_TYPE=1 ;;
		l ) LOG_DIR=$OPTARG ;;
		a ) ALT_DBNAME=$OPTARG ;;
		c ) DISTRIB=1 ;;
		f ) FULL_DISPLAY=1 ;;
		q ) unset VERBOSE ;;
		h ) DO_CHILD=1 ;;
		r ) RESPONSE_TIME=1 ;;
		D ) DEBUG_LEVEL=1 ;;
                * ) USAGE ;;
        esac
        done
	CHK_ON_PASSIVE_STANDBY
else
	CHK_ON_PASSIVE_STANDBY
	USAGE
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
CHK_DB_RUNNING
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
READ_CHK
if [ $READ_COUNT -ne 0 ];then
        ERROR_EXIT "[FATAL]:-This script cannot be used on a Master read only segment instance" 2
fi
if [ $SEG_TYPE -eq 1 ];then
	#Check to see that mirrors are configured
	CHK_MIRRORS_CONFIGURED
	if [ $MIRROR_COUNT -ne 2 ];then
		ERROR_EXIT "[FATAL]:-No mirror segments configured, -m option invalid" 2
	else
		LOG_MSG "[INFO]:-Mirror segments exist for use of -m option"
		TYPE_TEXT="Mirror Segments"
	fi
else
		TYPE_TEXT="Primary Segments"
fi
if [ x"" != x"$TARGETTBL" ];then
	GET_QD_DB_NAME
	if [ x"" != x"$ALT_DBNAME" ];then
        	QD_DBNAME=$ALT_DBNAME
	fi
	CHK_EXISTS_TABLE $TARGETTBL $MASTER_PORT $QD_DBNAME
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Table $TARGETTBL does not exist" 2
	else
		GET_DISTRIB_COLS $TARGETTBL $MASTER_PORT $QD_DBNAME
		if [ $DISTRIB -eq 1 ];then
			GET_DISTRIB_INFO
		else
			GET_TABLE_TYPE $TARGETTBL $MASTER_PORT $QD_DBNAME
			GET_NODE_COUNT
		fi
		if [ $DO_CHILD -eq 1 ] && [ $PARTITIONED == 't' ];then
			GET_CHILD_NAMES			
			LOG_MSG "[INFO]:-Processing child tables"
			for CHILD_TABLE_NAME in ${CHILD_NAME_ARRAY[@]}
			do
				TARGETTBL=${SCHEMA_NAME}.$CHILD_TABLE_NAME
				CHK_EXISTS_TABLE $TARGETTBL $MASTER_PORT $QD_DBNAME
				GET_DISTRIB_COLS $TARGETTBL $MASTER_PORT $QD_DBNAME
				if [ $DISTRIB -eq 1 ];then
					GET_DISTRIB_INFO
				else
					GET_TABLE_TYPE $TARGETTBL $MASTER_PORT $QD_DBNAME
					GET_NODE_COUNT
				fi
			done
		fi
	fi
else
	USAGE
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
