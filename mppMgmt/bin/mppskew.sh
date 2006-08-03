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
TIME=`$DATE +%H":"%M":"%S`
CUR_DATE=`$DATE +%Y%m%d`
MASTER_HOSTNAME=`hostname`
EXIT_STATUS=0
FULL_DISPLAY=0
DISTRIB=0
SEG_TYPE=0
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	GET_QD_DB_NAME >/dev/null 2>&1
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
        $ECHO "    Reports the data skew for a table within an MPP database."
	$ECHO "      Reports the segments with the most and least number of records,"
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
	$ECHO "      Logging options:"
	$ECHO "      -l, <logfile directory> alternative logfile directory [optional]"
	$ECHO "      -q, quiet mode, only useful where -f option also used"
	$ECHO
	$ECHO "      Skew options:"
	$ECHO "      -t, <table name> [mandatory]"
	$ECHO "      -a, <database name> alternative database name [default ${QD_DBNAME}]"
	$ECHO "      -f, display full results output [default summary]"
	$ECHO "      -c, display distribution columns for table, do not compute skew"
	$ECHO "      -m, use mirror segment hosts for skew analysis [default primary segments]"
	$ECHO
	$ECHO "      Connection options:"
        $ECHO "      -d, <master instance directory> name of alternative master instance to connect [optional]"
        $ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
}

GET_DISTRIB_INFO () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ `$ECHO $DISTRIB_COLS |$GREP -c "{"` -ne 0 ];then
		LOG_MSG "[INFO]:-Obtaining OID for $TABLENAME on QD"
		TABLE_OID=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select oid from pg_class where relname='$TABLENAME' and relkind='r' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
		ERROR_CHK $? "obtain table oid value" 2
	TOTAL_COLS=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select max(attnum) from pg_attribute where attrelid=${TABLE_OID};"`
	ERROR_CHK $? "obtain table column count" 2
	LOG_MSG "[INFO]:-Distribution columns for $TARGETTBL"
	LOG_MSG "[INFO]:-------------------------------------------"
	COLNUM=0
	for ATT_NUM in `echo $DISTRIB_COLS|$TR -d '{'|$TR -d '}'|$TR ',' ' '`
	do
		DIS_COL_NAME=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select attname from pg_attribute where attrelid=${TABLE_OID} and attnum=${ATT_NUM};"`
		((COLNUM=$COLNUM+1))
		LOG_MSG "[INFO]:-Distribution column $COLNUM at DDL position $ATT_NUM is named $DIS_COL_NAME"
	done
	LOG_MSG "[INFO]:-Total columns in table $TARGETTBL = $TOTAL_COLS"
	LOG_MSG "[INFO]:-------------------------------------------"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	if [ $PARTITIONED == 't' ];then
		LOG_MSG "[INFO]:-Table $TARGETTBL is type Partitioned"
		PARENT_TABLE_RELFILENODE=`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select relfilenode from pg_class where relname='$TABLENAME' and relkind='r' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
		CHILD_NAME_ARRAY=(`$PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select relname from pg_class where relkind='r' and relfilenode in (select inhrelid from pg_inherits where inhparent=$PARENT_TABLE_RELFILENODE);"|$TR '\n' ' '`)
		for C_NAME in ${CHILD_NAME_ARRAY[@]}
		do
			LOG_MSG "[INFO]:-Has child table $C_NAME"
		done	
	fi
	else
		LOG_MSG "[INFO]:-Distribution columns are of type special"
	fi
}
GET_NODE_COUNT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ `$ECHO $TARGETTBL|$GREP -c "\."` -eq 1 ];then
		SCHEMA_NAME=`$ECHO $TARGETTBL|$CUT -d. -f1`
		T_NAME=`$ECHO $TARGETTBL|$CUT -d. -f2`
	fi
	SEGCOUNT=0
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

	LOG_MSG  "[INFO]:-Sending request to nodes.."
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
	ERROR_CHK $? "test access to $SEG_PORT $QD_DBNAME on $SEG_HOST" 2
	$TRUSTED_SHELL -n $SEG_HOST "${EXPORT_LIB_PATH}; time env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT $QD_DBNAME -A -t -c'select count(*),'\''$SEG_PORT'\'','\''$SEG_NAME'\'','\''$SEG_HOST'\'' from $TARGETTBL;'" 2>&1|$HEAD -3|$TR -s '\n'|$TR ' ' '\n'|$TR -d '.'|$TR -cs '[:alnum:]' '[\n*]'|$GREP -v real|$TR  '\n' '|'|$AWK '{print $1}' >> $TMPFILE &
	ERROR_CHK $? "send request to $SEG_HOST port $SEG_PORT" 2
	let SEGCOUNT=$SEGCOUNT+1
	done
	LOG_MSG "[INFO]:-Requests sent.."
#Now wait until the temporary file contains $SegCount lines
	LOG_MSG  "[INFO]:-Waiting for results..."
	while [ $SEGCOUNT -ne `$WC -l $TMPFILE|$AWK '{print $1}'` ]
	do
		$SLEEP 1
	done
	REC_COUNT=0
	for RECS in `cat $TMPFILE|$CUT -d'|' -f1`
	do
		((REC_COUNT=$REC_COUNT+$RECS))
	done
	MinSkew=10000000000000
	MaxSkew=0
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
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Response results for $TYPE_TEXT"
	while read LINE
	do
		Host=`$ECHO $LINE|$CUT -d '|' -f4`
		Time=`$ECHO $LINE|$CUT -d '|' -f5`
		Seg=`$ECHO $LINE|$CUT -d '|' -f3`
		Port=`$ECHO $LINE|$CUT -d '|' -f2`
		LOG_MSG "[INFO]:-Host $Host segment $Seg port $Port responded in time $Time"
	done < $TMPFILE
	if [ $FULL_DISPLAY -eq 1 ] || [ x"" = x"$VERBOSE" ];then
		$CAT $TMPFILE|$SORT | $TEE -a $LOG_FILE
	else
		$CAT $TMPFILE|$SORT >> $LOG_FILE
	fi
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Table of results for Table $TARGETTBL in database $QD_DBNAME"
	if [ $PARTITIONED == 't' ];then
	LOG_MSG "[INFO]:-Table $TARGETTBL is type Partitioned"
	else
	LOG_MSG "[INFO]:-Table $TARGETTBL is type Un-Partitioned"
	fi
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Distribution columns = $DISTRIB_COLS"
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Total records = $REC_COUNT"
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Distribution for $TYPE_TEXT"
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Skew result"
	LOG_MSG "[INFO]:-$MaxHost segment $MaxSeg port $MaxPort has maximum count of $MaxSkew records"
	LOG_MSG "[INFO]:-$MinHost segment $MinSeg port $MinPort has minimum count of $MinSkew records"
	LOG_MSG "[INFO]:-Record count variance is $RecDiff records"
	LOG_MSG "-------------------------------------------------------------------"
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
        while getopts ":a:vmcqft:'?'d:l:" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                t ) TARGETTBL=`$ECHO $OPTARG|$TR '[A-Z]' '[a-z]'` ;;
                '?' ) USAGE ;;
                d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
		m ) SEG_TYPE=1 ;;
		l ) LOG_DIR=$OPTARG ;;
		a ) ALT_DBNAME=$OPTARG ;;
		c ) DISTRIB=1 ;;
		f ) FULL_DISPLAY=1 ;;
		q ) unset VERBOSE ;;
                * ) UNKNOWN_OPTION ;;
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
		GET_DISTRIB_COLS $TABLENAME $MASTER_PORT $QD_DBNAME
		if [ $DISTRIB -eq 1 ];then
			GET_DISTRIB_INFO
		else
		GET_NODE_COUNT
		fi
	fi
else
	USAGE
fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
