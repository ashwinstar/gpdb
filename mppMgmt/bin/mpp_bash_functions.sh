#!/bin/bash
#	Filename:-		mpp_bash_functions.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Updated:-		$Date$
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
#			mppheadsync.bsh utility.
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
CMDPATH=(/usr/kerberos/bin /usr/sfw/bin /opt/sfw/bin /usr/local/bin /bin /usr/bin /sbin /usr/sbin /usr/ucb)

#MPPPATH is the list of possible locations for the Bizgres binaries, in precedence order
declare -a MPPPATH
MPPPATH=( $MPPHOME $BIZHOME )
if [ ${#MPPPATH[@]} -eq 0 ];then
	echo "[FATAL]:-MPPHOME and BIZHOME are not set, need to set at least one of these within environment"
	exit 2
fi

#MPP_UNIQUE_COMMAND is used to identify the binary directory
MPP_UNIQUE_COMMAND=mppsyncmaster


findCmdInPath() {
		cmdtofind=$1

		for pathel in ${CMDPATH[@]}
				do
				CMD=$pathel/$cmdtofind
				if [ x"$CMD" != x"" ] && [ -f $CMD ]; then
						echo $CMD
						return
				fi
		done
		echo $cmdtofind
		return "Problem in mppbash_functions, command '$cmdtofind' not found in COMMAND path. You will need to edit the script named mppbash_functions.sh to properly locate the needed commands for your platform."
}

findMppPath() {
		cmdtofind=$MPP_UNIQUE_COMMAND

		for pathel in ${MPPPATH[@]}
				do
				CMD=`find $pathel -name $cmdtofind | tail -1`
				if [ x"$CMD" != x"" ] && [ -f $CMD ]; then
						echo $CMD
						return
				fi
		done
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
PROMPT="$ECHO"
#TABECHO="$ECHO -e \t"
#PROMPT="$ECHO -n -e \t"
GREP=`findCmdInPath grep`
GTAR=`findCmdInPath gtar`
EGREP=`findCmdInPath egrep`
HEAD=`findCmdInPath head`
HOSTNAME=`findCmdInPath hostname`
IPCS=`findCmdInPath ipcs`
IFCONFIG=`findCmdInPath ifconfig`
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
TAR=`findCmdInPath tar`
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
USER_NAME=`id|$AWK '{print $1}'|$CUT -d"(" -f2|$TR -d ')'`
PROG_NAME=`echo $0 | $TR -d '-'`
PROG_NAME=`$BASENAME $PROG_NAME`
CALL_HOST=`$HOSTNAME|$CUT -d. -f1`

#******************************************************************************
# Locate the postgres routines from the MPP release
#******************************************************************************
PSQLBIN=`findMppPath`

if [ x"$PSQLBIN" = x"" ];then
		echo "Problem in mppbash_functions, command '$MPP_UNIQUE_COMMAND' not found in MPP path. Try setting BIZHOME to the location of your MPP distribution"
		exit 99
fi

PSQLBIN=`$DIRNAME $PSQLBIN`


#******************************************************************************
# MPP Command Variables
#******************************************************************************
INITDB=$PSQLBIN/initdb
MPPLOADER=$CMDBIN/loader.sh
PG_CTL=$PSQLBIN/pg_ctl
PG_DUMP=$PSQLBIN/pg_dump
PG_RESTORE=$PSQLBIN/pg_restore
PSQL=$PSQLBIN/psql
MPP_DUMPALL=$PSQLBIN/mpp_dumpall
MPP_RESTORE=$PSQLBIN/mpp_restore
MPPSYNCMASTER=$PSQLBIN/mppsyncmaster

#******************************************************************************
# General Variables
#******************************************************************************
PG_PID=postmaster.pid
PG_OPT=postmaster.opts
PG_CONF=postgresql.conf
PG_HBA=pg_hba.conf
if [ x"$TRUSTED_SHELL" = x"" ]; then TRUSTED_SHELL="$SSH"; fi
if [ x"$TRUSTED_COPY" = x"" ]; then TRUSTED_COPY="$SCP"; fi
PG_CONF_ADD_FILE=$WORKDIR/postgresql_conf_mpp_additions
RECOVER_FLAG=/tmp/active_recovery_in_progress
SCHEMA_FILE=cdb_schema.sql
DEFAULTDB=template1
#******************************************************************************
# Functions
#******************************************************************************
CHK_DB_QD_SET () {
		if [ ! $MASTER_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-MASTER_DATA_DIRECTORY parameter not set, update user start-up file [i.e. .bashrc]." 2
		fi
}

VERSION_INFO () {
		VERSION=`$HEAD -10 $0|$GREP -i Version: | $CUT -d# -f2|$CUT -d- -f2|$TR -d '\t'`
		STATUS=`$HEAD -10 $0|$GREP -i Status: | $CUT -d# -f2|$CUT -d- -f2|$TR -d '\t'`
		UPDATE=`$HEAD -10 $0|$GREP -i Updated:| $CUT -d# -f2|$CUT -d- -f2|$TR -d '\t'`
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
		$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1 Script Exiting!" >> $LOG_FILE
		$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1 Script Exiting!"
		if [ $BACKOUT_FILE ]; then
				if [ -s $BACKOUT_FILE ]; then
						LOG_MSG "[WARN]:-Script has left MPP configuration in an incomplete state"
						LOG_MSG "[WARN]:-Run command /bin/bash $BACKOUT_FILE to remove these changes"
						BACKOUT_COMMAND "if [ $MASTER_HOSTNAME != \`$HOSTNAME\` ];then $ECHO \"[FATAL]:-Not on original master host $MASTER_HOSTNAME, backout script exiting!\";exit 2;fi"
						$ECHO "$RM -f $BACKOUT_FILE" >> $BACKOUT_FILE
				fi
		fi
		exit $2
}

ERROR_CHK () {
	if [ $# -ne 3 ];then 
		LOG_MSG "[WARN]:-Incorrect # parameters supplied to $FUNCTNAME"
		return;fi
	RETVAL=$1;shift
	MSG_TXT=$1;shift
	ACTION=$1 #1=issue warn, 2=fatal
	if [ $RETVAL -eq 0 ];then
		LOG_MSG "[INFO]:-Successfully completed $MSG_TXT"
	else
		if [ $ACTION -eq 1 ];then
			LOG_MSG "[WARN]:-Issue with $MSG_TXT"
			EXIT_STATUS=1
		else
			ERROR_EXIT "[FATAL]:-Failed to complete $MSG_TXT " 2
		fi
	fi	
}

ED_FILE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	ED_TMP_FILE=/tmp/ed_text.$$
	APPEND=0
	FILENAME=$1;shift
	SEARCH_TXT=$1;shift
	SUB_TXT="$1";shift
	KEEP_PREV=$1;shift
	ED_HOST=$1
	if [ x"" == x"$ED_HOST" ]; then
			if [ `$GREP -c "${SEARCH_TXT}" $FILENAME` -gt 1 ]; then
				LOG_MSG "[INFO]:-Found more than 1 instance of $SEARCH_TXT in $FILENAME, will append"
				APPEND=1
			fi
			if [ `$GREP -c "${SEARCH_TXT}" $FILENAME` -eq 0 ] || [ $APPEND -eq 1 ]; then
				$ECHO $SUB_TXT >> $FILENAME
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
					LOG_MSG "[WARN]:-Failed to append line $SUB_TXT to $FILENAME"
				else
					LOG_MSG "[INFO]:-Appended line $SUB_TXT to $FILENAME"
				fi
			else
if [ $KEEP_PREV -eq 0 ];then
ed $FILENAME << _EOF_ > /dev/null 2>&1
1
/${SEARCH_TXT}
.
i
$SUB_TXT #
.
.,.+1j
.
w
q
_EOF_
else
ed $FILENAME << _EOF_ > /dev/null 2>&1
1
/${SEARCH_TXT}
.
d
.
-1
a
$SUB_TXT
.
w
q
_EOF_
fi
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
					LOG_MSG "[WARN]:-Failed to replace $SEARCH_TXT in $FILENAME"
					ERROR_EXIT=1
				else
					LOG_MSG "[INFO]:-Replaced line in $FILENAME"
				fi
		fi
	else
		if [ `$TRUSTED_SHELL $ED_HOST "$GREP -c \"${SEARCH_TXT}\" $FILENAME"` -gt 1 ]; then
			LOG_MSG "[INFO]:-Found more than 1 instance of $SEARCH_TXT in $FILENAME on $ED_HOST, will append"
			APPEND=1
		fi
		if [ `$TRUSTED_SHELL $ED_HOST "$GREP -c \"${SEARCH_TXT}\" $FILENAME"` -eq 0 ] || [ $APPEND -eq 1 ]; then
			$TRUSTED_SHELL $ED_HOST "$ECHO $SUB_TXT >> $FILENAME"
			RETVAL=$?
			if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Failed to append line $SUB_TXT to $FILENAME on $ED_HOST"
				ERROR_EXIT=1
			else
				LOG_MSG "[INFO]:-Appended line $SUB_TXT to $FILENAME on $ED_HOST"
			fi
		else
			$ECHO 1 > $ED_TMP_FILE
			$ECHO "/${SEARCH_TXT}" >>  $ED_TMP_FILE
			$ECHO . >>  $ED_TMP_FILE
			if [ $KEEP_PREV -eq 0 ];then
			$ECHO i >> $ED_TMP_FILE
			$ECHO "$SUB_TXT #" >>  $ED_TMP_FILE
			$ECHO . >>  $ED_TMP_FILE
			$ECHO ".,.+1j" >> $ED_TMP_FILE
			$ECHO . >>  $ED_TMP_FILE
			else
			$ECHO d >> $ED_TMP_FILE
			$ECHO . >>  $ED_TMP_FILE
			$ECHO -1 >> $ED_TMP_FILE
			$ECHO a >> $ED_TMP_FILE
			$ECHO "$SUB_TXT" >>  $ED_TMP_FILE
			$ECHO . >>  $ED_TMP_FILE
			fi
			$ECHO w >>  $ED_TMP_FILE
			$ECHO q >>  $ED_TMP_FILE
			#$SCP $ED_TMP_FILE ${ED_HOST}:/tmp > /dev/null 2>&1
			$CAT $ED_TMP_FILE | $TRUSTED_SHELL ${ED_HOST} $DD of=$ED_TMP_FILE > /dev/null 2>&1
			$TRUSTED_SHELL $ED_HOST "ed $FILENAME < $ED_TMP_FILE" > /dev/null 2>&1
			RETVAL=$?
			if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Failed to insert $SUB_TXT in $FILENAME on $ED_HOST"
				ERROR_EXIT=1
			else
				LOG_MSG "[INFO]:-Replaced line in $FILENAME on $ED_HOST"
			fi
			$TRUSTED_SHELL $ED_HOST "$RM -f $ED_TMP_FILE"
			$RM -f $ED_TMP_FILE
		fi
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}


CHK_EXISTS_TABLE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		TABLENAME=$1
		MASTER_PORT=$2
		QD_DBNAME=$3
		LOG_MSG "[INFO]:-Checking for table $TABLENAME in database $QD_DBNAME"
		export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;$PSQL -q -p  $MASTER_PORT $QD_DBNAME -c"\d $TABLENAME;" -o /dev/null
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
		DIR_NAME=$1;shift
		DIR_HOST=$1
		if [ x"" == x"$DIR_HOST" ];then
			EXISTS=`if [ -d $DIR_NAME ];then $ECHO 0;else $ECHO 1;fi`
		else
			EXISTS=`$TRUSTED_SHELL $DIR_HOST "if [ -d $DIR_NAME ];then $ECHO 0;else $ECHO 1;fi"`
			ERROR_CHK $? "obtain directory details $DIR_NAME on $DIR_HOST" 1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_MASTER_PORT () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		MASTER_DATA_DIRECTORY=$1
		if [ x"" == x"$MASTER_DATA_DIRECTORY" ];then
			ERROR_EXIT "[FATAL]:-MASTER_DATA_DIRECTORY variable not set" 2;fi
		if [ ! -d $MASTER_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-No $MASTER_DATA_DIRECTORY directory" 2
		fi
		MASTER_PORT=`$GREP "^port" $MASTER_DATA_DIRECTORY/$PG_CONF |$GREP -v "^#"|$CUT -d= -f2|$AWK '{print $1}'`
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_ON_PASSIVE_STANDBY () {
	MPPSYNC_COUNT=0
	CHK_DB_QD_SET
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	GET_PG_PID_ACTIVE $MASTER_PORT
	if [ -f $MASTER_DATA_DIRECTORY/postmaster.opts ];then
		MPPSYNC_COUNT=`$GREP -c mppsync $MASTER_DATA_DIRECTORY/postmaster.opts`
	fi
	if [ $PID -ne 0 ] && [ $MPPSYNC_COUNT -ne 0 ];then
		LOG_MSG "[FATAL]:-Cannot run this script on a passive standby QD where there" 
		LOG_MSG "[FATAL]:-is a conflict with the current value of the MASTER_DATA_DIRECTORY"
		LOG_MSG "[FATAL]:-environment variable setting."
		ERROR_EXIT "[FATAL]:-Unable to process requested command" 2
	fi
}

CHK_DB_RUNNING () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ ! -d $MASTER_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-No Master $MASTER_DATA_DIRECTORY directory" 2
		fi
		if [ ! -f $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:-No $MASTER_DATA_DIRECTORY/$PG_PID file" 2
		fi		
		export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"\l" > /dev/null 2>&1
		if [ $? -ne 0 ];then
			LOG_MSG "[FATAL]:-Have a postmaster.pid file for QD instance on post $MASTER_PORT"
			ERROR_EXIT "[FATAL]:-However, error reported on test access to QD instance."
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QD_DB_NAME () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_MASTER_PORT $MASTER_DATA_DIRECTORY
		CHK_DB_RUNNING	
		QD_DBNAME=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"\l" 2>/dev/null|$GREP -v postgres|$GREP -v template|$HEAD -1|$CUT -d"|" -f1`
		ERROR_CHK $? "obtain database name" 2
		MASTER_DIR_CHK=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from mpp.host;"`
		if [ $MASTER_DIR_CHK -eq 0 ];then
			ERROR_EXIT "[FATAL]:-MASTER_DATA_DIRECTORY value of $MASTER_DATA_DIRECTORY is incorrect" 2;fi
		if [ x"" == x"$QD_DBNAME" ]; then
				LOG_MSG "[INFO]:-Unable to obtain Master database name, setting value to $DEFAULTDB"
				QD_DBNAME=$DEFAULTDB
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_STANDBY_COUNT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	STANDBY_COUNT=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from mpp.listener a, mpp.host b, mpp.mpp_pgdatabase c where a.listenerid in (select listenerid from mpp.mpp_pgdatabase where content=-1 and dbid<>1) and a.hostid=b.hostid and a.listenerid=c.listenerid;"`
	ERROR_CHK $? "obtain standby master host count" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_STANDBY_DETAILS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	STANDBY_ARRAY=(`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port, c.valid, c.definedprimary from mpp.listener a, mpp.host b, mpp.mpp_pgdatabase c where a.listenerid in (select listenerid from mpp.mpp_pgdatabase where content=-1 and dbid<>1) and a.hostid=b.hostid and a.listenerid=c.listenerid;"`) > /dev/null 2>&1
	RETVAL=$?
	if [ $RETVAL -ne 0 ]; then
		LOG_MSG "[INFO]:-Unable to obtain standby master details, error code $RETVAL returned"
		EXIT_STATUS=1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_ARRAY=(`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port, c.valid, c.definedprimary from mpp.listener a, mpp.host b, mpp.mpp_pgdatabase c where a.listenerid not in (select listenerid from mpp.mpp_pgdatabase where content=-1) and a.hostid=b.hostid and a.listenerid=c.listenerid;"`) > /dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance host details from Master db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_DB_LIST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	DB_ARRAY=(`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $DEFAULTDB -A -t -c"\l"|$GREP -v template0|$GREP -v postgres|$CUT -d"|" -f1`)
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_MIRRORS_CONFIGURED () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MIRROR_COUNT=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid)/count(distinct(content)) from mpp.pgdatabase where content<>-1;"`
	ERROR_CHK $? "obtain mirror count from master instance" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_PRIMARY_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_PRIMARY_ARRAY=(`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port, c.valid, c.definedprimary from mpp.listener a, mpp.host b, mpp.mpp_pgdatabase c where a.listenerid not in (select listenerid from mpp.mpp_pgdatabase where content=-1) and a.hostid=b.hostid and a.listenerid in (select listenerid from mpp.mpp_pgdatabase where definedprimary='t') and a.listenerid=c.listenerid order by a.datadir;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance primary host details from Master db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_MIRROR_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME

		QE_MIRROR_ARRAY=(`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select b.hostname, a.datadir, a.port, c.valid, c.isprimary from mpp.listener a, mpp.host b, mpp.mpp_pgdatabase c where a.listenerid not in (select listenerid from mpp.mpp_pgdatabase where content=-1) and a.hostid=b.hostid  and a.listenerid=c.listenerid and c.definedprimary='f' order by a.datadir;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance mirror host details from Master db, error code $RETVAL returned"
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_PG_PID_ACTIVE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		# Expects hostname and port number
		PORT=$1;shift
		HOST=$1
		if [ x"" == x"$HOST" ];then
			PG_LOCK_FILE=`$NETSTAT -a|$GREP ".s.PGSQL.${PORT}"|$GREP LISTENING|$AWK '{print $NF}'`
			if [ x"" == x"$PG_LOCK_FILE" ];then
				PID=0
				LOG_MSG "[INFO]:-No socket connection found for port=${PORT}"
			else
				if [ -f ${PG_LOCK_FILE}.lock ];then
					PID=`$CAT ${PG_LOCK_FILE}.lock|$HEAD -1|$AWK '{print $1}'`
					LOG_MSG "[INFO]:-PID found for $PORT PID=$PID"
				else
					PID=0
					LOG_MSG "[WARN]:-Socket connection found, but no ${PG_LOCK_FILE}.lock"
					EXIT_STATUS=1
				fi
			fi
		else
			PG_LOCK_FILE=`$TRUSTED_SHELL $HOST "$NETSTAT -a|$GREP ".s.PGSQL.${PORT}" 2>/dev/null"|$AWK '{print $NF}'`
			if [ x"" == x"$PG_LOCK_FILE" ];then
				PID=0
				LOG_MSG "[INFO]:-No socket connection found for port=${PORT} on $HOST"
			else
				if [ `$TRUSTED_SHELL $HOST "$LS ${PG_LOCK_FILE}.lock 2>/dev/null"|$WC -l` -ne 0 ];then
					PID=`$TRUSTED_SHELL $HOST "$CAT ${PG_LOCK_FILE}.lock|$HEAD -1 2>/dev/null"|$AWK '{print $1}'`
					LOG_MSG "[INFO]:-PID found for $PORT PID=$PID on $HOST"
				else
					PID=0
					LOG_MSG "[WARN]:-Socket connection found, but no ${PG_LOCK_FILE}.lock on $HOST"
					EXIT_STATUS=1
				fi
			fi
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RUN_COMMAND () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COMMAND="$1"
		LOG_MSG "[INFO]:-Commencing local $COMMAND"
		$COMMAND
		RETVAL=$?
		echo "Return code = $RETVAL"
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
		$TRUSTED_SHELL $HOST $COMMAND
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then 
			ERROR_EXIT "[FATAL]:- Command $COMMAND on $HOST failed with error status $RETVAL" 2
		else 
			LOG_MSG "[INFO]:-Completed $TRUSTED_SHELL $HOST $COMMAND"
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

PING_HOST () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	TARGET_HOST=$1
	$PING -c 1 $TARGET_HOST > /dev/null 2>&1
	RETVAL=$?
	case $RETVAL in
		0) LOG_MSG "[INFO]:-$TARGET_HOST contact established" ;;
		1) ERROR_EXIT "[FATAL]:-Unable to contact $TARGET_HOST" 2 ;;
		2) ERROR_EXIT "[FATAL]:-Unknown host $TARGET_HOST" 2 ;;
	esac
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	return $RETVAL
}

STANDBY_CATALOG_UPDATE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MASTER_STANDBY_HOSTNAME=$1;shift
	MASTER_STANDBY_DATABASE=$1;shift
	MASTER_STANDBY_DATA_DIRECTORY=$1;shift
	MASTER_STANDBY_PORT=$1
	STANDBY_HOST_COUNT=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"select count(*) from mpp.host where hostname='${MASTER_STANDBY_HOSTNAME}';"`
	ERROR_CHK $? "obtain standby host count" 2
        if [ $STANDBY_HOST_COUNT -eq 0 ];then
                LOG_MSG "[INFO]:-Creating new mpp.host record for ${MASTER_STANDBY_HOSTNAME}"
                MAX_ID=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"select max(hostid)+1 from mpp.host;"`
		ERROR_CHK $? "obtain mas hostid from mpp.host" 2
                export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"insert into mpp.host values (${MAX_ID},'${MASTER_STANDBY_HOSTNAME}');"
                ERROR_CHK $? "insert into mpp.host for standby host" 2
        else
                MAX_ID=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"select hostid from mpp.host where hostname='${MASTER_STANDBY_HOSTNAME}';"`
		ERROR_CHK $? "obtain hostid where hostname=${MASTER_STANDBY_HOSTNAME}" 2
        fi
        LOG_MSG "[INFO]:-Creating new mpp.listener record for ${MASTER_STANDBY_HOSTNAME}"
        MAX_LISTEN_ID=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"select max(listenerid)+1 from mpp.listener;"`
	ERROR_CHK $? "obtain max listenerid" 2
        export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"insert into mpp.listener values (${MAX_LISTEN_ID},${MAX_ID},${MASTER_STANDBY_PORT},'${MASTER_STANDBY_DATA_DIRECTORY}','${USER_NAME}');"
        ERROR_CHK $? "insert into mpp.lister for standby host" 2
        LOG_MSG "[INFO]:-Creating new mpp.pgdatabase record for ${MASTER_STANDBY_HOSTNAME}"
        MAX_DB_ID=`export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"select max(dbid)+1 from mpp.pgdatabase;"`
	ERROR_CHK $? "obtain max dbid from mpp.pgdatabase" 2
        export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $MASTER_STANDBY_DATABASE -A -t -c"insert into mpp.pgdatabase values (${MAX_DB_ID},'f',-1,${MAX_LISTEN_ID},'t','f');"
        ERROR_CHK $? "add standby host mpp.pgdatabase record" 2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

START_SYNC_PROCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 3 ];then ERROR_EXIT "[FATAL:-Expected 3 parameters got $# " 2;fi
	MASTER_STANDBY_HOSTNAME=$1;shift
	MASTER_STANDBY_DATA_DIRECTORY=$1;shift
	MASTER_STANDBY_PORT=$1
	#Check to see if already have a process running on this port
	GET_PG_PID_ACTIVE $MASTER_STANDBY_PORT ${MASTER_STANDBY_HOSTNAME}
	if [ $PID -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Process PID=$PID on port=$MASTER_STANDBY_PORT running on $MASTER_STANDBY_HOSTNAME" 2
	fi
	LOG_MSG "[INFO]:-Starting mppsyncmaster on standby host ${MASTER_STANDBY_HOSTNAME}"
	$TRUSTED_SHELL ${MASTER_STANDBY_HOSTNAME} "exec $MPPSYNCMASTER -D $MASTER_STANDBY_DATA_DIRECTORY -p $MASTER_STANDBY_PORT -h $MASTER_STANDBY_HOSTNAME  >> ${MASTER_STANDBY_DATA_DIRECTORY}.log 2>&1 &"
	ERROR_CHK $? "initialize mppsyncmaster on ${MASTER_STANDBY_HOSTNAME}" 1
	$SLEEP 3 #Wait to ensure process has time to start
	GET_PG_PID_ACTIVE $MASTER_STANDBY_PORT ${MASTER_STANDBY_HOSTNAME}
	if [ $PID -eq 0 ];then
		LOG_MSG "[WARN]:-Failed to start sync process on ${MASTER_STANDBY_HOSTNAME}"
		EXIT_STATUS=1
	else
		LOG_MSG "[INFO]:-Successfully started sync process on ${MASTER_STANDBY_HOSTNAME} PID=$PID"
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

STOP_SYNC_PROCESS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 2 ];then ERROR_EXIT "[FATAL:-Expected 2 parameters got $# " 2;fi
	MASTER_STANDBY_HOSTNAME=$1;shift
	MASTER_STANDBY_DATA_DIRECTORY=$1
	$TRUSTED_SHELL ${MASTER_STANDBY_HOSTNAME} "export LD_LIBRARY_PATH=${MPP_LIBRARY_PATH}:$LD_LIBRARY_PATH;$PG_CTL stop -D $MASTER_STANDBY_DATA_DIRECTORY -m immediate"
	ERROR_CHK $? "immediate shutdown of sync process on ${MASTER_STANDBY_HOSTNAME}" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}
#******************************************************************************
# Main Section
#******************************************************************************
#******************************************************************************
# Setup logging directory
#******************************************************************************
CUR_DATE=`$DATE +%Y%m%d`
DEFLOGDIR=$HOME/mppAdminLogs
if [ ! -d $DEFLOGDIR ]; then
		mkdir $DEFLOGDIR
fi
LOG_FILE=$DEFLOGDIR/${PROG_NAME}_${CUR_DATE}.log

if [ $# -ne 0 ]; then
		if [ $1 == "-v" ]; then
				VERSION_INFO
		fi
fi
#Set up OS type for scripts to change command lines
OS_TYPE=`uname -s|tr '[A-Z]' '[a-z]'`
case $OS_TYPE in
	sunos ) IFCONFIG_TXT="-a inet"
		PS_TXT="-ef" 
		DU_TXT="-s" ;;
	linux ) IFCONFIG_TXT="" 
		PS_TXT="ax" 
		DU_TXT="-c" ;;
	* ) echo unknown ;;
esac
#Set up MPP_LIBRARY_PATH
MPP_LIBRARY_PATH=`$DIRNAME \`$DIRNAME $INITDB\``/lib
export LD_LIBRARY_PATH=$MPP_LIBRARY_PATH:$LD_LIBRARY_PATH
SHARE_PATH=`$DIRNAME \`$DIRNAME $INITDB\``/share
SCRIPTS_DIR=`$DIRNAME \` $DIRNAME $INITDB\``/mppMgmt/bin
