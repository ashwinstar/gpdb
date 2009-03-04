#!/bin/bash
#	Filename:-		gpsepartw.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Dec 2006
#	Release stat:-		Released
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) 2007 Greenplum Inc
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code

WORKDIR=`dirname $0`

# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
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
# Level of script feedback 0=small 1=verbose
unset VERBOSE
GP_USER=$USER_NAME
EXIT_STATUS=0
TMP_COUNT_FILE=/tmp/${PROG_NAME}_${CUR_DATE}_TMP_COUNT_FILE_OUT.$$
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpcreatepart, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

INSERT_REC () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
#!/bin/bash
#	Filename:-		gpsepartw.sh
#	Version:-		$Revision$
#	Updated:-		$Date$	
#	Status:-		Released	
#	Author:-		G Coombe	
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		Dec 2006
#	Release stat:-		Released
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) 2007 Greenplum Inc
#******************************************************************************
# Update History
#******************************************************************************
# Ver	Date		Who		Update
#******************************************************************************
# Detailed Description
#******************************************************************************
#******************************************************************************
# Prep Code

WORKDIR=`dirname $0`

# Source required functions file, this required for script to run
# exit if cannot locate this file. Change location of FUNCTIONS variable
# as required.
FUNCTIONS=$WORKDIR/gp_bash_functions.sh
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
# Level of script feedback 0=small 1=verbose
unset VERBOSE
GP_USER=$USER_NAME
EXIT_STATUS=0
TMP_COUNT_FILE=/tmp/${PROG_NAME}_${CUR_DATE}_TMP_COUNT_FILE_OUT.$$
#******************************************************************************
# Functions
#******************************************************************************
USAGE () {
		$ECHO
		$ECHO "      `basename $0`"
		$ECHO
		$ECHO "      Script called by gpcreatepart, this should not be run directly"
		exit $EXIT_STATUS
}

CHK_CALL () {
	FILE_PREFIX=`$ECHO $PARALLEL_STATUS_FILE|$CUT -d"." -f1`
	if [ ! -f ${FILE_PREFIX}.$PARENT_PID ];then
		$ECHO "[FATAL]:-Not called from from correct parent program" 
		exit 2
	fi
}

INSERT_REC () {
	LOG_MSG "[INFO][$INST_COUNT]:-Start Function $FUNCNAME"
	$EXPORT_LIB_PATH;$PSQL -p $MASTER_PORT -A -t -d $QD_DBNAME -c "insert into $CHILD_TARGET select * from $SECURE_TABLE_NAME where $SELECT_CLAUSE ;" > $TMP_COUNT_FILE 2>&1
	if [ $? -ne 0 ];then
		$ECHO "FAILED:${QE_LINE}:0:INSERT_FAILED" >> $PARALLEL_STATUS_FILE
		$CAT $TMP_COUNT_FILE >> $LOG_FILE
		$RM -f $TMP_COUNT_FILE
		LOG_MSG "[INFO][$INST_COUNT]:-End Main"
		exit $?
	else
		C_COUNT=`$CAT $TMP_COUNT_FILE|$GREP INSERT|$AWK '{print $3}'`
		$CAT $TMP_COUNT_FILE >> $LOG_FILE
		$RM -f $TMP_COUNT_FILE
		QE_LINE=COMPLETED:${QE_LINE}:${C_COUNT}:INSERT_COMPLETED
	fi
	LOG_MSG "[INFO][$INST_COUNT]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
trap '$ECHO "KILLED|0|${QE_PORT}|${QE_BASE_DIR}|${QE_NAME}|0m0s|" >> $PARALLEL_STATUS_FILE;ERROR_EXIT "[FATAL]:-[$INST_COUNT]-Recieved INT or TERM signal" 2' INT TERM
while getopts ":v'?'" opt
	do
	case $opt in
		v ) VERSION_INFO ;;
		'?' ) USAGE ;;
		* ) USAGE 
	esac
done
#Now process supplied call parameters
PARENT_PID=$1;shift		#PID of gpstate process calling this script
CHK_CALL
MASTER_PORT=$1;shift		#Segment port
INST_COUNT=$1;shift		#Unique number for this parallel script, starts at 0
LOG_FILE=$1;shift		#Central utility log file
QD_DBNAME=$1;shift		#Database
CHILD_TARGET=$1;shift		#Child table into which data to be inserted
SECURE_TABLE_NAME=$1;shift	#Source table where data to be inserted from
CRITERIA_TEXT=$1;shift		#Where clause to use to select data for insert into child table
SELECT_CLAUSE=`$ECHO $CRITERIA_TEXT|$TR '~' ' '`
QE_LINE=${QD_DBNAME}:${CHILD_TARGET}:${SECURE_TABLE_NAME}
LOG_MSG "[INFO][$INST_COUNT]:-Start Main"
INSERT_REC
$ECHO "$QE_LINE" >> $PARALLEL_STATUS_FILE
LOG_MSG "[INFO][$INST_COUNT]:-End Main"
exit $EXIT_STATUS
