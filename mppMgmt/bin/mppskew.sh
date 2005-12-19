#!/bin/bash
#	Filename:-		mppskew.bsh
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
# Script checks to ensure that table entered as parameter exists.
# If table name exists calls Get_Node_Count function which does the following;
# 1. Obtains port number, host and database name for all nodes in the cluster
# 2. Issues a non-blocking select count(*) for table to each node
# 3. Waits for a response from all nodes in the cluster
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
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
	GET_QD_DB_NAME >/dev/null 2>&1
	$ECHO
        $ECHO "      `basename $0`"
	$ECHO
        $ECHO "      Reports data skew for a table within an MPP database."
	$ECHO "      Will report # records per segment instance, and the elapse"
	$ECHO "      time for each segment instance host to respond to request."
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
	$ECHO "      Skew options:"
	$ECHO "      -t, <table name> [mandatory]"
	$ECHO "      -a, <database name> alternative database name [default ${QD_DBNAME}]"
	$ECHO "      -f, display full results output [default summary]"
	$ECHO
	$ECHO "      Connection options:"
        $ECHO "      -d, <head database directory> name of alternative head database to connect [optional]"
        $ECHO
        $ECHO "      Return codes:"
        $ECHO "      0 No problems encountered with requested operation"
        $ECHO "      1 Warning generated"
        $ECHO "      2 Fatal error, unable to continue requested operation"
	$ECHO
        exit $EXIT_STATUS
}

GET_NODE_COUNT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	SCHEMA=`$ECHO $TARGETTBL|$CUT -d. -f1`
	T_NAME=`$ECHO $TARGETTBL|$CUT -d. -f2`
	SEGCOUNT=0
	NUM_SEGMENTS=`$PSQL -p $MASTER_PORT  $QD_DBNAME -A -t -c "select count(*) from mpp_configuration where content<>-1 and isprimary='t';"`
	ERROR_CHK $? "obtain number of segment instances" 2
	if [ $NUM_SEGMENTS = 1 ];then
		ERROR_EXIT "[FATAL]:-Only one node in cluster, skew not applicable" 2;fi 
		$PSQL -p $MASTER_PORT $QD_DBNAME -t -A -F"," -c"select a.hostname, b.port, b.datadir from mpp_configuration where content <> -1 and isprimary='t' ;" > $NODELIST
		ERROR_CHK $? "obtaib list of active segments" 2
	LOG_MSG  "[INFO]:-Sending request to nodes.."

	SEGCOUNT=0
	for NodeInfo in `$CAT $NODELIST`
	do
	SEG_HOST=`$ECHO $NodeInfo|$CUT -d, -f1`
	SEG_PORT=`$ECHO $NodeInfo|$CUT -d, -f2`
	SEG_DIR=`$ECHO $NodeInfo|$CUT -d, -f3`
	SEG_NAME=`$BASENAME $SEG_DIR`
	$TRUSTED_SHELL $SEG_HOST "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH; time env PGOPTIONS=\"-c mpp_session_role=utility\" $PSQL -p $SEG_PORT $QD_DBNAME -A -t -c'select count(*),'\''$SEG_NAME'\'','\''$SEG_HOST'\'' from $TARGETTBL;'" 2>&1|$HEAD -3|$TR -s '\n'|$TR ' ' '\n'|$TR -d '.'|$TR -cs '[:alnum:]' '[\n*]'|$GREP -v real|$TR  '\n' '|'|$AWK '{print $1}' >> $TMPFILE &
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
	MaxSkew=`$SORT $TMPFILE|$TAIL -1|$CUT -d'|' -f1`
	MaxSeg=`$SORT $TMPFILE|$TAIL -1|$CUT -d'|' -f2`
	MaxHost=`$SORT $TMPFILE|$TAIL -1|$CUT -d'|' -f3`
	MinSkew=`$SORT $TMPFILE|$HEAD -1|$CUT -d'|' -f1`
	MinSeg=`$SORT $TMPFILE|$HEAD -1|$CUT -d'|' -f2`
	MinHost=`$SORT $TMPFILE|$HEAD -1|$CUT -d'|' -f3`
	MinTime=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$HEAD -1|$CUT -d"|" -f1`
	MinTimeHost=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$HEAD -1|$CUT -d"|" -f2`
	MinTimeSeg=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$HEAD -1|$CUT -d"|" -f3`
	MaxTime=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$TAIL -1|$CUT -d"|" -f1`
	MaxTimeHost=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$TAIL -1|$CUT -d"|" -f2`
	MaxTimeSeg=`$CAT $TMPFILE|$AWK -F"|" '{print $4"|"$3"|"$2}'|$SORT|$TAIL -1|$CUT -d"|" -f3`
	let RecDiff=$MaxSkew-$MinSkew
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Table of results for Table $TARGETTBL in database $QD_DBNAME"
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Total records = $REC_COUNT"
	LOG_MSG "-------------------------------------------------------------------"
	if [ $FULL_DISPLAY -eq 1 ];then
		$CAT $TMPFILE|$SORT | $TEE -a $LOG_FILE
	else
		$CAT $TMPFILE|$SORT >> $LOG_FILE
	fi
	LOG_MSG "[INFO]:-Skew result"
	LOG_MSG "[INFO]:-Host $MaxHost segment $MaxSeg has maximum row count of $MaxSkew records"
	LOG_MSG "[INFO]:-Host $MinHost segment $MinSeg has minimum row count of $MinSkew records"
	LOG_MSG "[INFO]:-Record count variance is $RecDiff records"
	LOG_MSG "-------------------------------------------------------------------"
	LOG_MSG "[INFO]:-Response result"
	LOG_MSG "[INFO]:-Fastest host $MinTimeHost segment $MinTimeSeg responded in $MinTime"
	LOG_MSG "[INFO]:-Slowest host $MaxTimeHost segment $MaxTimeSeg responded in $MaxTime" 
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
        while getopts ":a:vft:'?'d:l:" opt;do
        case $opt in
                v ) VERSION_INFO ;;
                t ) TARGETTBL=$OPTARG ;;
                '?' ) USAGE ;;
                d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
		l ) LOG_DIR=$OPTARG ;;
		a ) ALT_DBNAME=$OPTARG ;;
		f ) FULL_DISPLAY=1 ;;
                * ) UNKNOWN_OPTION ;;
        esac
        done
else
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
CHK_ON_PASSIVE_STANDBY
CHK_DB_RUNNING
LOG_MSG "[INFO]:-Start Main"
LOG_MSG "[INFO]:-Command line options passed to utility = $*"
if [ $TARGETTBL ];then
GET_QD_DB_NAME
if [ x"" != x"$ALT_DBNAME" ];then
        QD_DBNAME=$ALT_DBNAME
fi
CHK_EXISTS_TABLE $TARGETTBL $MASTER_PORT $QD_DBNAME
if [ $EXISTS -ne 0 ];then
	ERROR_EXIT "ERROR:-Table $TARGETTBL does not exist" 2;fi
GET_NODE_COUNT
else
	USAGE;fi
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
