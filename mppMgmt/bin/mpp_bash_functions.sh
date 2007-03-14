#!/bin/bash
#	Filename:-		mpp_bash_functions.sh
#	Version:-		$Revision$
#	Updated:-		$Date$
#	Updated:-		$Date$
#	Status:-		Released	
#	Author:-		G L Coombe (Greenplum)
#	Contact:-		gcoombe@greenplum.com
#	Release date:-		March 2006
#	Release stat:-		Greenplum Internal
#                               Copyright (c) Metapa 2005. All Rights Reserved.
#                               Copyright (c) Greenplum 2005. All Rights Reserved
#	Brief descn:-		Common functions used by various scripts
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
#
#***************************************************************
# Location Functions
#******************************************************************************
#Check that SHELL is /bin/bash
	if [ $SHELL != /bin/bash ] && [ `ls -al /bin/sh|grep -c bash` -ne 1 ];then
		echo "[FATAL]:-Scripts must be run by a user account that has SHELL=/bin/bash"
		if [ -f /bin/bash ];then
			echo "[INFO]:-/bin/bash exists, please update user account shell"
		else
			echo "[WARN]:-/bin/bash does not exist, does bash need to be installed?"
		fi
		exit 2
	fi
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
		return "Problem in mpp_bash_functions, command '$cmdtofind' not found in COMMAND path. You will need to edit the script named mppbash_functions.sh to properly locate the needed commands for your platform."
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
FIND=`findCmdInPath find`
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
LOCALE=`findCmdInPath locale`
MAIL=`findCmdInPath mail`
MV=`findCmdInPath mv`
MORE=`findCmdInPath more`
MKDIR=`findCmdInPath mkdir`
MKFIFO=`findCmdInPath mkfifo`
NETSTAT=`findCmdInPath netstat`
PING=`findCmdInPath ping`
PS=`findCmdInPath ps`
RCP=`findCmdInPath rcp`
RSH=`findCmdInPath rsh`
RM=`findCmdInPath rm`
SCP=`findCmdInPath scp`
SED=`findCmdInPath sed`
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
		echo "Problem in mpp_bash_functions, command '$MPP_UNIQUE_COMMAND' not found in MPP path. Try setting BIZHOME to the location of your MPP distribution"
		exit 99
fi

PSQLBIN=`$DIRNAME $PSQLBIN`
SCRIPTDIR="`$DIRNAME $PSQLBIN`/mppMgmt/bin"
#******************************************************************************
# MPP Scripts
#******************************************************************************
MPPACTIVATEMASTER=$SCRIPTDIR/mppactivatemaster.sh
MPPACTIVATESTANDBY=$SCRIPTDIR/mppactivatestandby.sh
MPPADDMIRRORS=$SCRIPTDIR/mppaddmirrors.sh
MPPCREATECLUSTER=$SCRIPTDIR/mppcreatecluster.sh
MPPCRONDUMP=$SCRIPTDIR/mppcrondump.sh
MPPDELETECLUSTER=$SCRIPTDIR/mppdeletecluster.sh
MPPINITSTANDBY=$SCRIPTDIR/mppinitstandby.sh
MPPREBUILDCLUSTER=$SCRIPTDIR/mpprebuildcluster.sh
MPPREBUILDSEG=$SCRIPTDIR/mpprebuildseg.sh
MPPRECOVERSEG=$SCRIPTDIR/mpprecoverseg.sh
MPPSIZECALC=$SCRIPTDIR/mppsizecalc.sh
MPPSKEW=$SCRIPTDIR/mppskew.sh
MPPSTART=$SCRIPTDIR/mppstart.sh
MPPSTATE=$SCRIPTDIR/mppstate.sh
MPPSTOP=$SCRIPTDIR/mppstop.sh
MAILFILE=$SCRIPTDIR/mail_contacts
HMAILFILE=$HOME/mail_contacts
MPPDOCDIR=${MPPHOME}/mppMgmt/doc
#******************************************************************************
# MPP Command Variables
#******************************************************************************
INITDB=$PSQLBIN/initdb
MPPLOADER=$CMDBIN/loader.sh
PG_CTL=$PSQLBIN/pg_ctl
PG_DUMP=$PSQLBIN/pg_dump
PG_RESTORE=$PSQLBIN/pg_restore
PSQL=$PSQLBIN/psql
MPP_DUMP=$PSQLBIN/mpp_dump
MPP_RESTORE=$PSQLBIN/mpp_restore
MPPSYNCMASTER=$PSQLBIN/mppsyncmaster
#******************************************************************************
# MPP OS Settings 
#******************************************************************************
OS_OPENFILES=65535
#******************************************************************************
# General Variables
#******************************************************************************
HOSTFILE=/etc/hosts
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
CONFIG_TABLE=mpp_configuration
MPP_MPP_TABLE=mpp_mpp
MPP_PG_VIEW=mpp_pgdatabase
MPP_INITDB_VER_TABLE=mpp_version_at_initdb
DEFAULT_CHK_PT_SEG=8
DEFAULT_IP_ALLOW="0.0.0.0/0"
DEFAULT_QD_MAX_CONNECT=25
QE_CONNECT_FACTOR=5
# DEFAULT_BUFFERS sets the default shared_buffers unless overridden by '-b'.
# It applies to the master db and segment dbs.  Specify either the number of
# buffers (without suffix) or the amount of memory to use for buffers (with
# case-insensitive suffix 'kB', 'MB' or 'GB').
DEFAULT_BUFFERS=128000kB
DEBUG_LEVEL=0
BATCH_DEFAULT=60
WAIT_LIMIT=300
WARN_MARK="<<<<<"
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
		DISPLAY_TXT=0
		if [ x"" == x"$DEBUG_LEVEL" ];then
			DEBUG_LEVEL=1
		fi
		if [ $# -eq 2 ];then 
			DISPLAY_TXT=1
		fi
		if [ $VERBOSE ]; then
				if [ $DEBUG_LEVEL -eq 1 ] || [ $DISPLAY_TXT -eq 1 ];then
			        $ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1" | $TEE -a $LOG_FILE	
				else
				$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1" >> $LOG_FILE
				fi
		else
				$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1" >> $LOG_FILE
		fi
}


ERROR_EXIT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		TIME=`$DATE +%H":"%M":"%S`
		CUR_DATE=`$DATE +%Y%m%d`
		$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1 Script Exiting!" >> $LOG_FILE
		$ECHO "${CUR_DATE}:${TIME}:${PROG_NAME}:${CALL_HOST}:${USER_NAME}-$1 Script Exiting!"
		DEBUG_LEVEL=1
		if [ $BACKOUT_FILE ]; then
				if [ -s $BACKOUT_FILE ]; then
						LOG_MSG "[WARN]:-Script has left MPP configuration in an incomplete state"
						LOG_MSG "[WARN]:-Run command /bin/bash $BACKOUT_FILE to remove these changes"
						BACKOUT_COMMAND "if [ $MASTER_HOSTNAME != \`$HOSTNAME\` ];then $ECHO \"[FATAL]:-Not on original master host $MASTER_HOSTNAME, backout script exiting!\";exit 2;fi"
						$ECHO "$RM -f $BACKOUT_FILE" >> $BACKOUT_FILE
				fi
		fi
		exit $2
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

READ_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	READ_COUNT=`env PGOPTIONS="-c mpp_session_role=utility" $PSQL -A -t -p $MASTER_PORT $DEFAULTDB -c" select count(*) from $CONFIG_TABLE where port<0;"`
	LOG_MSG "[INFO]:-Obtained -ve port read count $READ_COUNT"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

ERROR_CHK () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 3 ];then
		INITIAL_LEVEL=$DEBUG_LEVEL
		DEBUG_LEVEL=1
		LOG_MSG "[WARN]:-Incorrect # parameters supplied to $FUNCNAME"
		DEBUG_LEVEL=$INITIAL_LEVEL
		return;fi
	RETVAL=$1;shift
	MSG_TXT=$1;shift
	ACTION=$1 #1=issue warn, 2=fatal
	if [ $RETVAL -eq 0 ];then
		LOG_MSG "[INFO]:-Successfully completed $MSG_TXT"
	else
		if [ $ACTION -eq 1 ];then
			INITIAL_LEVEL=$DEBUG_LEVEL
			DEBUG_LEVEL=1
			LOG_MSG "[WARN]:-Issue with $MSG_TXT"
			EXIT_STATUS=1
			DEBUG_LEVEL=$INITIAL_LEVEL
		else
			LOG_MSG "[INFO]:-End Function $FUNCNAME"
			ERROR_EXIT "[FATAL]:-Failed to complete $MSG_TXT " 2
		fi
	fi	
	LOG_MSG "[INFO]:-End Function $FUNCNAME"	
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
				LOG_MSG "[INFO]:-Found more than 1 instance of $SEARCH_TXT in $FILENAME, will append" 1
				APPEND=1
			fi
			if [ `$GREP -c "${SEARCH_TXT}" $FILENAME` -eq 0 ] || [ $APPEND -eq 1 ]; then
				$ECHO $SUB_TXT >> $FILENAME
				RETVAL=$?
				if [ $RETVAL -ne 0 ]; then
					LOG_MSG "[WARN]:-Failed to append line $SUB_TXT to $FILENAME" 1
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
			LOG_MSG "[INFO]:-Found more than 1 instance of $SEARCH_TXT in $FILENAME on $ED_HOST, will append" 1
			APPEND=1
		fi
		if [ `$TRUSTED_SHELL $ED_HOST "$GREP -c \"${SEARCH_TXT}\" $FILENAME"` -eq 0 ] || [ $APPEND -eq 1 ]; then
			$TRUSTED_SHELL $ED_HOST "$ECHO $SUB_TXT >> $FILENAME"
			RETVAL=$?
			if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Failed to append line $SUB_TXT to $FILENAME on $ED_HOST"
				ERROR_EXIT=1
			else
				LOG_MSG "[INFO]:-Appended line $SUB_TXT to $FILENAME on $ED_HOST" 1
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
		if [ $# -ne 3 ];then
		ERROR_EXIT "[FATAL]:-Incorrect number of parameters passed expected 3 got $#" 2;fi
		DB_THERE=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $DEFAULTDB -c"select count(*) from pg_database where datname='$QD_DBNAME';"`
		if [ $DB_THERE -eq 0 ];then
			ERROR_EXIT "[FATAL]:-Database $QD_DBNAME does not exist" 2;fi
		LOG_MSG "[INFO]:-Checking for table $TABLENAME in database $QD_DBNAME"
		if [ `$ECHO $TABLENAME|$GREP -c "\."` -eq 1 ];then
			SCHEMA_NAME=`$ECHO $TABLENAME|$CUT -d"." -f1`
                        TABLENAME=`$ECHO $TABLENAME|$CUT -d"." -f2`
			TAB_COUNT=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $QD_DBNAME -c"select count(*) from pg_class where relname='$TABLENAME' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
			if [ $TAB_COUNT -eq 1 ];then
			PARTITIONED=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $QD_DBNAME -c"select relhassubclass from pg_class where relname='$TABLENAME' and relnamespace in (select oid from pg_namespace where nspname='$SCHEMA_NAME');"`
			fi
		else
			TAB_COUNT=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $QD_DBNAME -c"select count(*) from pg_class where relname='$TABLENAME';"`
			if [ $TAB_COUNT -eq 1 ];then
			PARTITIONED=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $QD_DBNAME -c"select relhassubclass from pg_class where relname='$TABLENAME';"`
			SCHEMA_NAME=`$EXPORT_LIB_PATH;$PSQL -A -t -q -p  $MASTER_PORT $QD_DBNAME -c"select nspname from pg_namespace where oid in(select relnamespace from pg_class where relname='$TABLENAME');"`
			fi
		fi
#		$EXPORT_LIB_PATH;$PSQL -q -p  $MASTER_PORT $QD_DBNAME -c"\d $TABLENAME;" -o /dev/null
		if [ $TAB_COUNT -eq 0 ] || [ $TAB_COUNT -gt 1 ];then
			EXISTS=1
		else
			EXISTS=0
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

EVENT_MAIL () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MAIL_SUBJECT=$1;shift
	MAIL_MESSAGE=$1
	if [ ! -f $MAILFILE ] && [ ! -f $HMAILFILE ] ;then
		LOG_MSG "[WARN]:-No $MAILFILE" 1
		LOG_MSG "[WARN]:-or $HMAILFILE" 1
		LOG_MSG "[WARN]:-Unable to send email notification" 1
		LOG_MSG "[INFO]:-To enable email notification, create $MAILFILE" 1
		LOG_MSG "[INFO]:-or $HMAILFILE containing required email addresses" 1
		EXIT_STATUS=1
	else
		if [ -f $HMAILFILE ];then MAILFILE=$HMAILFILE;fi
		for MAIL_ADDRESS in `$CAT $MAILFILE | $GREP -v "^#"`
		do
			LOG_MSG "[INFO]:-Sending mail to $MAIL_ADDRESS using file $MAILFILE" 1
			$ECHO "$MAIL_MESSAGE"|$MAIL -s "$MAIL_SUBJECT" $MAIL_ADDRESS
			ERROR_CHK $? "send email to $MAIL_ADDRESS" 1
		done
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_DISTRIB_COLS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 3 ];then
		ERROR_EXIT "[FATAL]:-Incorrect # parameters supplied, got $# expected 3" 2;fi
	T_NAME=$1
	if [ `$ECHO $1|$GREP -c "\."` -eq 1 ];then
		T_NAME=`$ECHO $1|$CUT -d"." -f2`
		S_NAME=`$ECHO $1|$CUT -d"." -f1`
		DISTRIB_COLS=`${EXPORT_LIB_PATH};$PSQL -p $2 $3 -A -t -c"select attrnums from  mpp_distribution_policy  where localoid in (select oid from pg_class where relname='${T_NAME}' and relnamespace in (select oid from pg_namespace where nspname='${S_NAME}'));"`
		if [ x"" == x"$DISTRIB_COLS" ];then
			DISTRIB_COLS='';fi
	else
		LOG_MSG "[WARN]:-Incorrect table name sent to function, should be schema.tablename" 1
		ERROR_EXIT "[FATAL]:-Error in $FUNCNAME parameters" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_TABLE_TYPE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 3 ];then
		ERROR_EXIT "[FATAL]:-Incorrect # parameters supplied, got $# expected 3" 2;fi
	T_NAME=$1
	if [ `$ECHO $1|$GREP -c "\."` -eq 1 ];then
		T_NAME=`$ECHO $1|$CUT -d"." -f2`
		S_NAME=`$ECHO $1|$CUT -d"." -f1`
		TAB_TYPE=`${EXPORT_LIB_PATH};$PSQL -p $2 $3 -A -t -c"select count(*) from pg_depend where objid in (select oid from pg_class where relname='${T_NAME}' and relnamespace in (select oid from pg_namespace where nspname='${S_NAME}')) and refobjid not in (select relnamespace from pg_class where relname='${T_NAME}' and relnamespace in (select oid from pg_namespace where nspname='${S_NAME}'));"`
		if [ $TAB_TYPE -eq 1 ];then
			TAB_TYPE_TXT="Child table"
			TAB_PARENT_NAME=(`${EXPORT_LIB_PATH};$PSQL -p $2 $3 -R" " -A -t -c"select relname from pg_class where oid in (select refobjid from pg_depend  where objid in (select oid from pg_class where relname='${T_NAME}' and relnamespace in (select oid from pg_namespace where nspname='${S_NAME}')) and refobjid not in (select relnamespace from pg_class where relname='${T_NAME}' and relnamespace in (select oid from pg_namespace where nspname='${S_NAME}')));"`)
		else
			TAB_TYPE_TXT="Not child table"
		fi
	else
		LOG_MSG "[WARN]:-Incorrect table name sent to function, should be schema.tablename" 1
		ERROR_EXIT "[FATAL]:-Error in $FUNCNAME parameters" 2
	fi
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
			RETVAL=$?
			if [ $RETVAL -ne 0 ];then
			LOG_MSG "[WARN]:-Failed to obtain details of $DIR_NAME on $DIR_HOST" 1
			EXIT_STATUS=1
			EXISTS=1
			fi
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_WRITE_DIR () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	DIR_NAME=$1;shift
	DIR_HOST=$1
	LOG_MSG "[INFO]:-Checking write access to $DIR_NAME on $DIR_HOST"
	$TRUSTED_SHELL $DIR_HOST "$TOUCH ${DIR_NAME}/tmp_file_test"
	RETVAL=$?
	if [ $RETVAL -ne 0 ];then
		ERROR_EXIT "[FATAL]:-Cannot write to $DIR_NAME on $DIR_HOST " 2
	else
		$TRUSTED_SHELL $DIR_HOST "$RM -f ${DIR_NAME}/tmp_file_test"
	LOG_MSG "[INFO]:-Write test passed on $DIR_HOST $DIR_NAME directory"
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
		if [ -r $MASTER_DATA_DIRECTORY/$PG_CONF ];then
			MASTER_PORT=`$AWK 'split($0,a,"#")>0 && split(a[1],b,"=")>1 {print b[1] " " b[2]}' $MASTER_DATA_DIRECTORY/$PG_CONF | $AWK '$1=="port" {print $2}' | $TAIL -1`
			if [ x"" == x"$MASTER_PORT" ];then
			ERROR_EXIT "[FATAL]:-Failed to obtain master port number from $MASTER_DATA_DIRECTORY/$PG_CONF" 2
			fi
		else
			ERROR_EXIT "[FATAL]:-Do not have read access to $MASTER_DATA_DIRECTORY/$PG_CONF" 2
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_ON_PASSIVE_STANDBY () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MPPSYNC_COUNT=0
	CHK_DB_QD_SET
	GET_MASTER_PORT $MASTER_DATA_DIRECTORY
	GET_PG_PID_ACTIVE $MASTER_PORT
	if [ -f $MASTER_DATA_DIRECTORY/postmaster.opts ];then
		MPPSYNC_COUNT=`$GREP -c mppsync $MASTER_DATA_DIRECTORY/postmaster.opts`
	fi
	if [ $PID -ne 0 ] && [ $MPPSYNC_COUNT -ne 0 ];then
		LOG_MSG "[FATAL]:-Cannot run this script on a passive standby instance" 1
		LOG_MSG "[FATAL]:-where there is a conflict with the current value of" 1
		LOG_MSG "[FATAL]:-the MASTER_DATA_DIRECTORY environment variable setting." 1
		ERROR_EXIT "[FATAL]:-Unable to process requested command" 2
	fi
	if [ $MPPSYNC_COUNT -ne 0 ];then
		LOG_MSG "[FATAL]:-Cannot run this script on the standby instance" 1
		LOG_MSG "[FATAL]:-Status indicates that standby instance processs not running" 1
		LOG_MSG "[FATAL]:-Check standby process status via mppstate.sh -f on Master instance" 1
		ERROR_EXIT "[FATAL]:-Unable to process requested command" 2
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

BUILD_MASTER_PG_HBA_FILE () {
        LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -eq 0 ];then ERROR_EXIT "[FATAL]:-Passed zero parameters, expected at least 1" 2;fi
	MPP_DIR=$1
	if [ x"" == x"$2" ];then
		IP_ALLOW=$DEFAULT_IP_ALLOW
	else
		IP_ALLOW=$2
	fi
        LOG_MSG "[INFO]:-Clearing values in Master $PG_HBA"
        $GREP "^#" ${MPP_DIR}/$PG_HBA > $TMP_PG_HBA
        $MV $TMP_PG_HBA ${MPP_DIR}/$PG_HBA
        LOG_MSG "[INFO]:-Setting local access"
        $ECHO "local    all         $USER_NAME         $PG_METHOD" >> ${MPP_DIR}/$PG_HBA
        $ECHO "local    all         all                $PG_METHOD" >> ${MPP_DIR}/$PG_HBA
        LOG_MSG "[INFO]:-Setting local host access"
        $ECHO "host     all         $USER_NAME         127.0.0.1/32    trust" >> ${MPP_DIR}/$PG_HBA

        $ECHO "#Master IP addresses " >> ${MPP_DIR}/$PG_HBA
        for ADDR in "${MASTER_IP_ADDRESS_ALL[@]}"
        do
        	$ECHO "host     all         $USER_NAME         $ADDR/32       trust" >> ${MPP_DIR}/$PG_HBA
        done

        $ECHO "#Standby Master IP addressess " >> ${MPP_DIR}/$PG_HBA
        for ADDR in "${STANDBY_IP_ADDRESS_ALL[@]}"
        do
        	$ECHO "host     all         $USER_NAME         $ADDR/32       trust" >> ${MPP_DIR}/$PG_HBA
        done

        # Add all local IPV6 addresses
        $ECHO "#IPV6 local" >> ${MPP_DIR}/$PG_HBA
        for ADDR in "${MASTER_IPV6_LOCAL_ADDRESS_ALL[@]}"
        do
        	$ECHO "host     all         $USER_NAME         $ADDR/128       trust" >> ${MPP_DIR}/$PG_HBA
        done
        $ECHO "#other " >> ${MPP_DIR}/$PG_HBA


        $ECHO "host     all         $USER_NAME         $IP_ALLOW       md5" >> ${MPP_DIR}/$PG_HBA
        $ECHO "host     all         all                $IP_ALLOW       trust" >> ${MPP_DIR}/$PG_HBA
        LOG_MSG "[INFO]:-Complete Master $PG_HBA configuration"
        LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

CHK_DB_RUNNING () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		if [ ! -d $MASTER_DATA_DIRECTORY ]; then
				ERROR_EXIT "[FATAL]:-No Master $MASTER_DATA_DIRECTORY directory" 2
		fi
		if [ ! -f $MASTER_DATA_DIRECTORY/$PG_PID ]; then
				ERROR_EXIT "[FATAL]:-No $MASTER_DATA_DIRECTORY/$PG_PID file" 2
		fi		
		GET_MASTER_PORT $MASTER_DATA_DIRECTORY
		export $EXPORT_LIB_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"\l" >> $LOG_FILE 2>&1
		if [ $? -ne 0 ];then
			LOG_MSG "[FATAL]:-Have a postmaster.pid file for master instance on port $MASTER_PORT" 1
			LOG_MSG "[FATAL]:-However, error reported on test psql access to master instance" 1
			LOG_MSG "[INFO]:-Check ps output for a postmaster process on the above port" 1
			LOG_MSG "[INFO]:-Check the master postgres logfile for errors and also the utility log file" 1
			ERROR_EXIT "[FATAL]:-Unable to continue" 2
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QD_DB_NAME () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_MASTER_PORT $MASTER_DATA_DIRECTORY
		CHK_DB_RUNNING	
		#Check if we have PGDATABASE environment variable set, if so see if that database exists
		if [ x"" != x"$PGDATABASE" ];then
			LOG_MSG "[INFO]:-PGDATABASE set, checking for this database"
			QD_DBNAME_THERE=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from pg_database where datname='${PGDATABASE}';"`
			ERROR_CHK $? "check for $PGDATABASE" 2
			if [ $QD_DBNAME_THERE -eq 1 ];then
				QD_DBNAME=$PGDATABASE
			else
				QD_DBNAME_THERE=""
			fi
		fi
		if [ x"" = x"$QD_DBNAME_THERE" ];then
			LOG_MSG "[INFO]:-Checking for a non-system database"
			QD_DBNAME=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"\l" 2>/dev/null|$GREP -v postgres|$GREP -v template0|$HEAD -1|$CUT -d"|" -f1`
			ERROR_CHK $? "obtain database name" 2
		fi
		MASTER_DIR_CHK=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(*) from ${CONFIG_TABLE};"`
		if [ $MASTER_DIR_CHK -eq 0 ];then
			ERROR_EXIT "[FATAL]:-MASTER_DATA_DIRECTORY value of $MASTER_DATA_DIRECTORY is incorrect" 2;fi
		if [ x"" == x"$QD_DBNAME" ]; then
				LOG_MSG "[INFO]:-Unable to obtain a non-system database name, setting value to $DEFAULTDB"
				QD_DBNAME=$DEFAULTDB
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_STANDBY_COUNT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	STANDBY_COUNT=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select count(*) from $CONFIG_TABLE where content=-1 and dbid<>1 ;"`
	ERROR_CHK $? "obtain standby master host count" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_STANDBY_DETAILS () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	GET_QD_DB_NAME
	STANDBY_ARRAY=(`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select hostname, datadir, port, valid, definedprimary from $CONFIG_TABLE where content=-1 and dbid<>1"`) > /dev/null 2>&1
	RETVAL=$?
	if [ $RETVAL -ne 0 ]; then
		LOG_MSG "[INFO]:-Unable to obtain standby master details, error code $RETVAL returned" 1
		EXIT_STATUS=1
	fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_TRANS_READ () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	TRANS_STATE=`${EXPORT_LIB_PATH};$PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"show transaction_read_only;"|$AWK '{print $NF}'`
	ERROR_CHK $? "obtain transaction state" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_ARRAY=(`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select a.hostname, a.datadir, a.port, b.valid, b.definedprimary from $CONFIG_TABLE a, $MPP_PG_VIEW b where a.dbid=b.dbid and a.content<>-1;"`) > /dev/null 2>&1
		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance host details from Master db, error code $RETVAL returned" 1
				EXIT_STATUS=1
		fi
		QE_ARRAY_COUNT=${#QE_ARRAY[@]}
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
	MIRROR_COUNT=`${EXPORT_LIB_PATH};$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(dbid)/count(distinct(content)) from $CONFIG_TABLE where content<>-1;"`
	ERROR_CHK $? "obtain mirror count from master instance" 2
	LOG_MSG "[INFO]:-Obtained $MIRROR_COUNT as check value"
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_MIRROR_TYPE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MIR_NUM=`${EXPORT_LIB_PATH};$PSQL -p $MASTER_PORT $DEFAULTDB -A -t -c"select count(distinct hostname) - (select count(distinct hostname) from $CONFIG_TABLE where content<>-1 and hostname not in (select hostname from $CONFIG_TABLE where definedprimary='t' and content<>-1)) from $CONFIG_TABLE where content in (select content from $CONFIG_TABLE where hostname in (select hostname from $CONFIG_TABLE where content=0 and definedprimary='t') and content <>-1 and definedprimary='f');"`
	if [ $MIR_NUM -eq 0 ];then
		MIR_TYPE="No Mirror";MIR_TYPE_NUM=0;fi
	if [ $MIR_NUM -ge 1 ] && [ $MIR_NUM -le 2 ];then
		MIR_TYPE="Group";MIR_TYPE_NUM=1;fi
	if [ $MIR_NUM -ge 3 ];then
		MIR_TYPE="Spread";MIR_TYPE_NUM=2;fi
	if [ $MIR_NUM -lt 0 ];then
		MIR_TYPE="Separate Hosts";MIR_TYPE_NUM=3;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_PRIMARY_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_PRIMARY_ARRAY=(`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select a.hostname, a.datadir, a.port, b.valid, b.isprimary from $CONFIG_TABLE a, $MPP_PG_VIEW b where a.dbid=b.dbid and a.content<>-1 and  a.definedprimary='t' order by a.content;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance primary host details from Master db, error code $RETVAL returned" 1
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_QE_MIRROR_DETAILS () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		GET_QD_DB_NAME
		QE_MIRROR_ARRAY=(`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -q -p $MASTER_PORT $QD_DBNAME -A -t -c"select a.hostname, a.datadir, a.port, b.valid, b.isprimary from $CONFIG_TABLE a, $MPP_PG_VIEW b where a.dbid=b.dbid and a.content<>-1 and a.definedprimary='f' order by a.content;" 2>/dev/null`) > /dev/null 2>&1

		RETVAL=$?
		if [ $RETVAL -ne 0 ]; then
				LOG_MSG "[WARN]:-Unable to obtain segment instance mirror host details from Master db, error code $RETVAL returned" 1
				EXIT_STATUS=1
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

GET_PG_PID_ACTIVE () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		# Expects port number and hostname for remote checking
		PORT=$1;shift
		HOST=$1
		PG_LOCK_FILE="/tmp/.s.PGSQL.${PORT}.lock"
		if [ x"" == x"$HOST" ];then
			#See if we have a netstat entry for this local host
			PG_LOCK_NETSTAT=`$NETSTAT -an 2>/dev/null |$GREP ".s.PGSQL.${PORT}"|$AWK '{print $NF}'|$HEAD -1`
			#See if we have a lock file in /tmp
			if [ -f ${PG_LOCK_FILE} ];then
				PG_LOCK_TMP=1
			else
				PG_LOCK_TMP=0
			fi
			if [ x"" == x"$PG_LOCK_NETSTAT" ] && [ $PG_LOCK_TMP -eq 0 ];then
				PID=0
				LOG_MSG "[INFO]:-No socket connection or lock file in /tmp found for port=${PORT}"
			else
				#Now check the failure combinations
				if [ $PG_LOCK_TMP -eq 0 ] && [ x"" != x"$PG_LOCK_NETSTAT" ];then
				#Have a process but no lock file
					LOG_MSG "[WARN]:-No lock file $PG_LOCK_FILE but process running on port $PORT" 1
					PID=1
					EXIT_STATUS=1
				fi
				if [ $PG_LOCK_TMP -eq 1 ] && [ x"" == x"$PG_LOCK_NETSTAT" ];then
				#Have a lock file but no process
					if [ -r ${PG_LOCK_FILE} ];then
						PID=`$CAT ${PG_LOCK_FILE}|$HEAD -1|$AWK '{print $1}'`
					else
						LOG_MSG "[WARN]:-Unable to access ${PG_LOCK_FILE}" 1
						PID=1
					fi
					LOG_MSG "[WARN]:-Have lock file $PG_LOCK_FILE but no process running on port $PORT" 1
					EXIT_STATUS=1
				fi
				if [ $PG_LOCK_TMP -eq 1 ] && [ x"" != x"$PG_LOCK_NETSTAT" ];then
				#Have both a lock file and a netstat process
					if [ -r ${PG_LOCK_FILE} ];then
						PID=`$CAT ${PG_LOCK_FILE}|$HEAD -1|$AWK '{print $1}'`
					else
						LOG_MSG "[WARN]:-Unable to access ${PG_LOCK_FILE}" 1
						PID=1
						EXIT_STATUS=1
					fi
					LOG_MSG "[INFO]:-Have lock file $PG_LOCK_FILE and a process running on port $PORT"
				fi
			fi
		else
			PING_HOST $HOST 1
			if [ $RETVAL -ne 0 ];then
				PID=0
				EXIT_STATUS=1
			else	
				PG_LOCK_NETSTAT=`$TRUSTED_SHELL $HOST "$NETSTAT -an 2>/dev/null |$GREP ".s.PGSQL.${PORT}" 2>/dev/null"|$AWK '{print $NF}'|$HEAD -1`
				PG_LOCK_TMP=`$TRUSTED_SHELL $HOST "$LS ${PG_LOCK_FILE} 2>/dev/null"|$WC -l`
				if [ x"" == x"$PG_LOCK_NETSTAT" ] && [ $PG_LOCK_TMP -eq 0 ];then
					PID=0
					LOG_MSG "[INFO]:-No socket connection or lock file $PG_LOCK_FILE found for port=${PORT}"
					EXIT_STATUS=1
				else
				#Now check the failure combinations
					if [ $PG_LOCK_TMP -eq 0 ] && [ x"" != x"$PG_LOCK_NETSTAT" ];then
					#Have a process but no lock file
						LOG_MSG "[WARN]:-No lock file $PG_LOCK_FILE but process running on port $PORT on $HOST" 1
						PID=1
						EXIT_STATUS=1
					fi
					if [ $PG_LOCK_TMP -eq 1 ] && [ x"" == x"$PG_LOCK_NETSTAT" ];then
					#Have a lock file but no process
						CAN_READ=`$TRUSTED_SHELL $HOST "if [ -r ${PG_LOCK_FILE} ];then echo 1;else echo 0;fi"`
						if [ $CAN_READ -eq 1 ];then
							PID=`$TRUSTED_SHELL $HOST "$CAT ${PG_LOCK_FILE}|$HEAD -1 2>/dev/null"|$AWK '{print $1}'`
						else
							LOG_MSG "[WARN]:-Unable to access ${PG_LOCK_FILE} on $HOST" 1
						fi
						LOG_MSG "[WARN]:-Have lock file $PG_LOCK_FILE but no process running on port $PORT on $HOST" 1
						EXIT_STATUS=1
					fi
					if [ $PG_LOCK_TMP -eq 1 ] && [ x"" != x"$PG_LOCK_NETSTAT" ];then
					#Have both a lock file and a netstat process
						CAN_READ=`$TRUSTED_SHELL $HOST "if [ -r ${PG_LOCK_FILE} ];then echo 1;else echo 0;fi"`
						if [ $CAN_READ -eq 1 ];then
							PID=`$TRUSTED_SHELL $HOST "$CAT ${PG_LOCK_FILE}|$HEAD -1 2>/dev/null"|$AWK '{print $1}'`
						else
							LOG_MSG "[WARN]:-Unable to access ${PG_LOCK_FILE} on $HOST" 1	
							EXIT_STATUS=1
						fi
						LOG_MSG "[INFO]:-Have lock file $PG_LOCK_FILE and a process running on port $PORT on $HOST"
					fi				
				fi
			fi
		fi
		LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

RUN_COMMAND () {
		LOG_MSG "[INFO]:-Start Function $FUNCNAME"
		COMMAND="$1"
		LOG_MSG "[INFO]:-Commencing local $COMMAND"
		$COMMAND >> $LOG_FILE 2>&1
		RETVAL=$?
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
		$TRUSTED_SHELL $HOST $COMMAND >> $LOG_FILE 2>&1
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
	TARGET_HOST=$1;shift
	PING_EXIT=$1
	if [ x"" == x"$PING_EXIT" ];then PING_EXIT=0;fi
	$PING -c 1 $TARGET_HOST > /dev/null 2>&1
	RETVAL=$?
	case $RETVAL in
		0) LOG_MSG "[INFO]:-$TARGET_HOST contact established" ;;
		1) if [ $PING_EXIT -eq 0 ];then
			ERROR_EXIT "[FATAL]:-Unable to contact $TARGET_HOST" 2
		   else
		        LOG_MSG "[WARN]:-Unable to contact $TARGET_HOST" 1
		   fi ;;
		2) if [ $PING_EXIT -eq 0 ];then
		 	ERROR_EXIT "[FATAL]:-Unknown host $TARGET_HOST" 2
		   else
			LOG_MSG "[WARN]:-Unknown host $TARGET_HOST" 1
		   fi ;;
	esac
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
	return $RETVAL
}

STANDBY_CATALOG_UPDATE () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	MASTER_STANDBY_HOSTNAME=$1;shift
	MASTER_STANDBY_DATA_DIRECTORY=$1;shift
	MASTER_STANDBY_PORT=$1
	STANDBY_HOST_COUNT=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select count(distinct hostname) from $CONFIG_TABLE where hostname='${MASTER_STANDBY_HOSTNAME}';"`
	ERROR_CHK $? "obtain standby host count" 2
        LOG_MSG "[INFO]:-Creating new $CONFIG_TABLE record for ${MASTER_STANDBY_HOSTNAME}"
        MAX_DB_ID=`${EXPORT_LIB_PATH};env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"select max(dbid)+1 from $CONFIG_TABLE;"`
	ERROR_CHK $? "obtain max dbid from $CONFIG_TABLE" 2
        $EXPORT_LIB_PATH;env PGOPTIONS="-c mpp_session_role=utility" $PSQL -p $MASTER_PORT -A -t $QD_DBNAME -c"insert into $CONFIG_TABLE (dbid, isprimary, content, valid, definedprimary, hostname, port, datadir) values (${MAX_DB_ID},'f',-1,'t','f','${MASTER_STANDBY_HOSTNAME}',${MASTER_STANDBY_PORT},'${MASTER_STANDBY_DATA_DIRECTORY}');"
        ERROR_CHK $? "add standby host $CONFIG_TABLE record" 2
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
		LOG_MSG "[INFO]:-Standby Master sync process active, will shutdown"
		STOP_SYNC_PROCESS $MASTER_STANDBY_HOSTNAME $MASTER_STANDBY_DATA_DIRECTORY
	fi
	LOG_MSG "[INFO]:-Starting mppsyncmaster on standby host ${MASTER_STANDBY_HOSTNAME}" 1
        $TRUSTED_SHELL ${MASTER_STANDBY_HOSTNAME} "exec $MPPSYNCMASTER -D $MASTER_STANDBY_DATA_DIRECTORY -p $MASTER_STANDBY_PORT -i  >> ${MASTER_STANDBY_DATA_DIRECTORY}.log 2>&1 &"
	$SLEEP 3 #Wait to ensure process has time to start
	GET_PG_PID_ACTIVE $MASTER_STANDBY_PORT ${MASTER_STANDBY_HOSTNAME}
	if [ $PID -eq 0 ];then
		LOG_MSG "[WARN]:-Failed to start sync process on ${MASTER_STANDBY_HOSTNAME}" 1
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
	LOG_MSG "[INFO]:-Stopping mppsyncmaster on standby host ${MASTER_STANDBY_HOSTNAME}" 1
	$TRUSTED_SHELL ${MASTER_STANDBY_HOSTNAME} "$EXPORT_LIB_PATH;$PG_CTL stop -D $MASTER_STANDBY_DATA_DIRECTORY -m immediate"
	RETVAL=$?
	ERROR_CHK $RETVAL "immediate shutdown of sync process on ${MASTER_STANDBY_HOSTNAME}" 1
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PARALLEL_SETUP () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	PARALLEL_STATUS_FILE=$1
	$TOUCH $PARALLEL_STATUS_FILE
	export PARALLEL_STATUS_FILE=$PARALLEL_STATUS_FILE
	LOG_MSG "[INFO]:-Spawning parallel processes    batch [1], please wait..." 1
	BATCH_COUNT=0
	INST_COUNT=0
	BATCH_DONE=1
	BATCH_TOTAL=0
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PARALLEL_COUNT () {
        LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	if [ $# -ne 2 ];then ERROR_EXIT "[FATAL]:-Incorrect number of parameters passed to $FUNCNAME" 2;fi
	BATCH_LIMIT=$1
	BATCH_DEFAULT=$2
	((INST_COUNT=$INST_COUNT+1))
	((BATCH_COUNT=$BATCH_COUNT+1))
	((BATCH_TOTAL=$BATCH_TOTAL+1))
	if [ $BATCH_COUNT -eq $BATCH_DEFAULT ] || [ $BATCH_LIMIT -eq $BATCH_TOTAL ];then
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $ECHO;fi
		PARALLEL_WAIT
		((BATCH_DONE=$BATCH_DONE+1))
		BATCH_COUNT=0
		if [ $BATCH_LIMIT -ne $BATCH_TOTAL ];then
			LOG_MSG "[INFO]:-Spawning parallel processes    batch [$BATCH_DONE], please wait..." 1
		fi
	fi
        LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PARALLEL_WAIT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"
	LOG_MSG "[INFO]:-Waiting for parallel processes batch [$BATCH_DONE], please wait..." 1
	SLEEP_COUNT=0
	while [ `$WC -l $PARALLEL_STATUS_FILE|$AWK '{print $1}'` -ne $INST_COUNT ]
	do
		if [ $DEBUG_LEVEL -eq 0 ] && [ x"" != x"$VERBOSE" ];then $NOLINE_ECHO ".\c";fi
		$SLEEP 1
		((SLEEP_COUNT=$SLEEP_COUNT+1))
		if [ $WAIT_LIMIT -lt $SLEEP_COUNT ];then
			if [ $DEBUG_LEVEL -eq 0 ];then $NOLINE_ECHO ".\c";fi
			LOG_MSG "[FATAL]:-Failed to process this batch of segments within $WAIT_LIMIT seconds" 1
			LOG_MSG "[INFO]:-Review contents of $LOG_FILE" 1
			ERROR_EXIT "[FATAL]:-Process timeout failure" 2
		fi
	done
	if [ $DEBUG_LEVEL -eq 0 ];then $ECHO;fi
	LOG_MSG "[INFO]:-End Function $FUNCNAME"
}

PARALLEL_SUMMARY_STATUS_REPORT () {
	LOG_MSG "[INFO]:-Start Function $FUNCNAME"	
	REPORT_FAIL=0
	if [ -f $1 ];then
	        KILLED_COUNT=`$GREP -c "KILLED:" $PARALLEL_STATUS_FILE`
                COMPLETED_COUNT=`$GREP -c "COMPLETED:" $PARALLEL_STATUS_FILE`
                FAILED_COUNT=`$GREP -c "FAILED:" $PARALLEL_STATUS_FILE`
                LOG_MSG "[INFO]:------------------------------------------------" 1
                LOG_MSG "[INFO]:-Parallel process exit status" 1
                LOG_MSG "[INFO]:------------------------------------------------" 1
                LOG_MSG "[INFO]:-Total processes marked as completed           = $COMPLETED_COUNT" 1
                if [ $KILLED_COUNT -ne 0 ];then
                LOG_MSG "[WARN]:-Total processes marked as killed              = $KILLED_COUNT $WARN_MARK" 1
		REPORT_FAIL=1
                else
                LOG_MSG "[INFO]:-Total processes marked as killed              = 0" 1
                fi
                if [ $FAILED_COUNT -ne 0 ];then
                LOG_MSG "[WARN]:-Total processes marked as failed              = $FAILED_COUNT $WARN_MARK" 1
		REPORT_FAIL=1
                else
                LOG_MSG "[INFO]:-Total processes marked as failed              = 0" 1
                fi
                LOG_MSG "[INFO]:------------------------------------------------" 1
	else
		 LOG_MSG "[WARN]:-Could not locate status file $1" 1
	fi
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
		LIB_TYPE="LD_LIBRARY_PATH"
		PG_METHOD=trust
		HOST_ARCH_TYPE="uname -i"
		NOLINE_ECHO=$ECHO
		DEFAULT_LOCALE_SETTING=en_US.UTF-8
		DU_TXT="-s" ;;
	linux ) IFCONFIG_TXT=""
		PS_TXT="ax"
		LIB_TYPE="LD_LIBRARY_PATH"
		PG_METHOD="ident sameuser"
		HOST_ARCH_TYPE="uname -i"
		NOLINE_ECHO="$ECHO -e"
		DEFAULT_LOCALE_SETTING=en_US.utf8
		DU_TXT="-c" ;;
	darwin ) IFCONFIG_TXT=""
		PS_TXT="ax"
		LIB_TYPE="DYLD_LIBRARY_PATH"
		PG_METHOD="ident sameuser"
		HOST_ARCH_TYPE="uname -m"
		NOLINE_ECHO=$ECHO
		DEFAULT_LOCALE_SETTING=en_US.utf-8
		DU_TXT="-c" ;;	
	freebsd ) IFCONFIG_TXT=""
		PS_TXT="ax"
		LIB_TYPE="LD_LIBRARY_PATH"
		PG_METHOD="ident sameuser"
		HOST_ARCH_TYPE="uname -m"
		NOLINE_ECHO="$ECHO -e"
		DEFAULT_LOCALE_SETTING=en_US.utf8
		DU_TXT="-c" ;;	
	* ) echo unknown ;;
esac
#Set up MPP_LIBRARY_PATH
MPP_LIBRARY_PATH=`$DIRNAME \`$DIRNAME $INITDB\``/lib
export LD_LIBRARY_PATH=$MPP_LIBRARY_PATH:$LD_LIBRARY_PATH
SHARE_PATH=`$DIRNAME \`$DIRNAME $INITDB\``/share
SCRIPTS_DIR=`$DIRNAME \` $DIRNAME $INITDB\``/mppMgmt/bin

export $LIB_TYPE=$MPP_LIBRARY_PATH:\$$LIB_TYPE
EXPORT_LIB_PATH='export '${LIB_TYPE}'='$MPP_LIBRARY_PATH':$'$LIB_TYPE
