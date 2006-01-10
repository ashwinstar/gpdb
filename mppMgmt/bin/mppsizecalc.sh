#!/bin/bash
#       Filename:-		mppsizecalc.sh
#       Version:-		$Revision$
#       Updated:-		$Date$
#       Status:-		MPP Conversion SE Co Internal
#       Author:-		G Coombe
#       Contact:-               gcoombe@greenplum.com
#       Release date:-          TBC
#       Release stat:-          Greenplum Co internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#       Brief descn:-           Starts the cluster database.
#******************************************************************************
# Update History
#******************************************************************************
# Ver   Date            Who             Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#
#******************************************************************************
# Prep Code
# Source required functions file, this required for script to run
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
QD_NAME=`hostname`
EXIT_STATUS=0
TMPFILE=/tmp/table_size.$$
SCALE=k
DISPLAY_TYPE=0
SIZE_TYPE=0 #0=table 1=index
#***********************************************************************************
USAGE () {
	GET_QD_DB_NAME >/dev/null 2>&1
	$ECHO 
	$ECHO "      `basename $0`"
	$ECHO
	$ECHO "      Provides actual disk space usage for a postgres table/index or database name supplied"
	$ECHO "      as a parameter to this script"
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
	$ECHO 
	$ECHO "      Size options:"
	$ECHO "      -t, <table name> name of table to obtain size data [mandatory if -x not supplied]"
	$ECHO "      -s, <scale> valid values b (bytes) k (kbytes) m (mbytes) [default:- ${SCALE}]"
	$ECHO "          where OS allows this option, otherwise defaults to KBytes"
	$ECHO "      -f, display full size information [default:-display total only]"
	$ECHO "      -a, <database name> alternative database name [default ${QD_DBNAME}]"
	$ECHO "      -i, obtain index size for <table name> [default:- table size]"
	$ECHO "      -x, <database name> name of database to obtain size data [default ${QD_DBNAME}]"
	$ECHO
	$ECHO "      Connection options:"
	$ECHO "      -d, <Master database directory> name of alternative Master database to start up [optional]"
	$ECHO
	$ECHO "      Return codes:"
	$ECHO "      0 No problems encountered with requested operation"
	$ECHO "      1 Warning generated, but operation completed"
	$ECHO "      2 Fatal error, unable to complete requested operation"
	$ECHO
	exit 0
}

GET_DB_OID_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	DB_OID=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select oid from pg_database where datname='${QD_DBNAME}';"`
	ERROR_CHK $? "obtain database OID from $QD_DBNAME database" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_TABLE_OID_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ `$ECHO $TABLE_NAME|$GREP -c .` -ne 0 ];then
		TABLE_NAME_NO_SCHEMA=`$ECHO $TABLE_NAME|$CUT -d. -f2`
		LOG_MSG "[INFO]:-Stripped off schema name"
	else
		TABLE_NAME_NO_SCHEMA=$TABLE_NAME
	fi
	TABLE_OID=`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select relfilenode from pg_class where relname='${TABLE_NAME_NO_SCHEMA}'"`
	ERROR_CHK $? "obtain table OID for $TABLE_NAME_NO_SCHEMA table in database $QD_DBNAME" 2	
	LOG_MSG "[INFO]:-Obtained OID value $TABLE_OID"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_OID_DATA () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_DB_OID_DATA
	GET_TABLE_OID_DATA
	if [ $SIZE_TYPE -eq 1 ]; then
		INDEX_OID=(`$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select indexrelid from pg_index where indrelid=${TABLE_OID};"`)
		INDEX_COUNT=${#INDEX_OID[@]}
		if [ $INDEX_COUNT -eq 0 ];then
			ERROR_EXIT "[FATAL]:-No indexes found for table $TABLE_NAME" 2
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

WAIT_COMPLETE () {
	while [ ! -f $TMPFILE ]
	do
		$SLEEP 1
	done
	while [ $LINECOUNT -ne `$WC -l $TMPFILE|$AWK '{print $1}'` ]
	do
		if [ $VERBOSE ];then $ECHO "processing...";fi
		$SLEEP 1
	done
	if [ $VERBOSE ];then $ECHO ;fi
}
	
SEND_TABLE_SIZE_REQUEST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	QE_COUNT=${#QE_ARRAY[@]}
	LOG_MSG "[INFO]:-Please wait, sending request to $QE_COUNT segment hosts"
	LINECOUNT=0
	for I in "${QE_ARRAY[@]}"
	do
		QE_HOST=`$ECHO $I|$CUT -d'|' -f1`
		QE_DIR=`$ECHO $I|$CUT -d'|' -f2`
		QE_TABLE=${QE_DIR}/base/${DB_OID}/${TABLE_OID}
		QE_BASE_DIR=`$BASENAME $QE_DIR`
		$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"`$TRUSTED_SHELL $QE_HOST "$DU -${SCALE} $DU_TXT ${QE_TABLE}*"|$TAIL -1|$AWK '{print \$1}'` >> $TMPFILE &
		((LINECOUNT=$LINECOUNT+1))
	done	
	WAIT_COMPLETE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SEND_INDEX_SIZE_REQUEST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	QE_COUNT=${#QE_ARRAY[@]}
	LOG_MSG "[INFO]:-Please wait, sending request to $QE_COUNT segment hosts"
	LINECOUNT=0
	for I in "${QE_ARRAY[@]}"
	do
		QE_HOST=`$ECHO $I|$CUT -d'|' -f1`
		QE_DIR=`$ECHO $I|$CUT -d'|' -f2`
		QE_BASE_DIR=`$BASENAME $QE_DIR`
		for IDX in "${INDEX_OID[@]}"
		do
			QE_INDEX=${QE_DIR}/base/${DB_OID}/$IDX
			$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"`$TRUSTED_SHELL $QE_HOST "$DU -${SCALE} $DU_TXT ${QE_INDEX}*"|$TAIL -1|$AWK '{print \$1}'` >> $TMPFILE &
			((LINECOUNT=$LINECOUNT+1))
		done
	done
	WAIT_COMPLETE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

SEND_DB_SIZE_REQUEST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	QE_COUNT=${#QE_ARRAY[@]}
	LOG_MSG "[INFO]:-Please wait, sending request to $QE_COUNT segment hosts"
	LINECOUNT=0
	for I in "${QE_ARRAY[@]}"
	do
		QE_HOST=`$ECHO $I|$CUT -d'|' -f1`
		QE_DIR=`$ECHO $I|$CUT -d'|' -f2`
		QE_DB=${QE_DIR}/base/${DB_OID}
		QE_BASE_DIR=`$BASENAME $QE_DIR`
		$ECHO ${QE_HOST}"|"${QE_BASE_DIR}"|"`$TRUSTED_SHELL $QE_HOST "$DU -${SCALE} $DU_TXT ${QE_DB}"|$TAIL -1 |$AWK '{print \$1}'`>> $TMPFILE &
		((LINECOUNT=$LINECOUNT+1)) 
	done	
	WAIT_COMPLETE
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CALC_TOTAL_SIZE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	TOTAL_SIZE=0
	for I in `$CAT $TMPFILE` 
	do
		SIZE=`$ECHO $I|$CUT -d'|' -f3`
		HOST=`$ECHO $I|$CUT -d'|' -f1`
		SEG=`$ECHO $I|$CUT -d'|' -f2`
		if [ $DISPLAY_TYPE -eq 1 ];then
			$ECHO "[INFO]:-Host $HOST Segment $SEG Space Used = $SIZE ${SCALE}"
		fi
		((TOTAL_SIZE=$TOTAL_SIZE+$SIZE))	
	done
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

DISPLAY_SIZE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	case $SCALE in
		b ) SCALE=Bytes ;;
		k ) SCALE=KBytes ;;
		m ) SCALE=MBytes ;;
	esac
	if [ $VERBOSE ];then
		case $SIZE_TYPE in 
			0) LOG_MSG "[INFO]:-Total space used for $TABLE_NAME table in database $QD_DBNAME " ;;
			1) LOG_MSG "[INFO]:-Total space used for indexes on $TABLE_NAME table in database $QD_DBNAME " ;;
			2) LOG_MSG "[INFO]:-Total space used for database $QD_DBNAME " ;;
		esac
		LOG_MSG "[INFO]:- $TOTAL_SIZE ${SCALE}"	
	else
		$ECHO $TOTAL_SIZE
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#***********************************************************************************
trap '$RM -f $TMPFILE;ERROR_EXIT "[FATAL]:-Recieved INT or TERM signal" 2' INT TERM
CHK_DB_QD_SET
if [ $# -ne 0 ]; then
	while getopts ":fvt:'?'ql:d:s:a:ix:" opt;do
	case $opt in
		v ) VERSION_INFO ;;
		t ) TABLE_NAME=$OPTARG ;;
		'?' ) USAGE ;;
		d ) MASTER_DATA_DIRECTORY=$OPTARG ;;
		q ) unset VERBOSE ;;
		l ) LOG_DIR=$OPTARG ;;
		s ) SCALE=$OPTARG ;;
		f ) DISPLAY_TYPE=1 ;;
		a ) ALT_DBNAME=$OPTARG ;;
		i ) SIZE_TYPE=1 ;;
		x ) SIZE_DB=$OPTARG ;;
		* ) USAGE ;;
		esac
	done
else
	USAGE
fi
if [ $SCALE != "k" ] && [ $OS_TYPE != "sunos" ];then
	case $SCALE in
		m ) ;;
		b ) ;;
		* ) ERROR_EXIT "[FATAL]:-Parameter -s must be either b, k or m" 2 ;;
	esac
else
	SCALE="k"
fi
CHK_ON_PASSIVE_STANDBY
if [ x"" == x"$TABLE_NAME" ] && [ x"" == x"$SIZE_DB" ];then
	ERROR_EXIT "[FATAL]:-Must supply a table name via -t option, or use -x <database name> option" 2
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
GET_QD_DB_NAME
GET_QE_DETAILS
if [ x"" != x"$ALT_DBNAME" ];then
	QD_DBNAME=$ALT_DBNAME
fi
if [ x"" != x"$SIZE_DB" ];then
	QD_DBNAME=$SIZE_DB
	SIZE_TYPE=2
fi
if [ $SIZE_TYPE -le 1 ];then
	CHK_EXISTS_TABLE $TABLE_NAME $MASTER_PORT $QD_DBNAME
	if [ $EXISTS -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Table $TABLE_NAME does not exist in $QD_DBNAME database" 2
	fi
	GET_OID_DATA
	if [ $SIZE_TYPE -eq 0 ];then
		SEND_TABLE_SIZE_REQUEST
	else
		SEND_INDEX_SIZE_REQUEST
	fi
else
	GET_DB_OID_DATA
	SEND_DB_SIZE_REQUEST
fi
CALC_TOTAL_SIZE
DISPLAY_SIZE
$RM -f $TMPFILE
LOG_MSG "[INFO]:-End Main"
exit $EXIT_STATUS
