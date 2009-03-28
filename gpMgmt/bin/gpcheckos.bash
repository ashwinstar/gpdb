#!/bin/bash

#
# GPCheckOS Script v1.3
#
# This script is intended to replace the previous GPCheckOS written in Python.
# Use this script to identify discrepancies between the current installation
# and the appliance standards.  Please note that the presence of a discrepancy
# is not, in and of itself, a problem.  Many sites have intentionally 
# configured in a non-standard way.
#
# This version of this script must be run locally.  The next version will
# support remote execution.
#
# This script requires the gpcheck-`uname -s`.res file.  If this file is not 
# in the same path as the script itself, the script will use the following 
# information to find it in order:
#
#  1. the path named in the -c switch
#  2. $GPHOME/etc/gpcheckos
#  3. /etc/gpcheckos
#
# If the resource file cannot be found, an error will be shown.
#
# CHANGELOG:
#
# 1.1 -
#    -added support for -q to the parse_res function
#
#    -changed some of the output statements to override -q when present.
#
# 1.2 -
#    -moved some output statements under there the LOG_FILE variable is set
#     so that the lnout function from the library can be used to manage them.
#
# 1.3 - 
#    -put in checks for the output folder and moved existing ones out of the way
#     prior to beginning.
#
#    -Moved the log checks to the top of the execution order 
#
#    -Changed how TEMP_DIR is handled.  Moves existing one out of the way

#Script version output with the -v option
VERSION=1.3

#Flag to indicate when to show version information
SHOW_VERS=1

#RUN_NM is used to add an identifier to the execution results.
RUN_NM=${$}

#ITER is used in the csv for identification
ITER=0

#CONFIG_DIR is used as the location for the resource file.
CONFIG_DIR=${GPHOME}/bin/lib

#LOG_DIR is used as the location for storing output files.  A 
#        sub-folder using the value of RUN_NM will be created and
#        all files placed there.
LOG_DIR=/var/tmp

#RES_FILE_NM names the resource file
RES_FILE_NM=gpcheckos-`uname -s`.res

#RES_PATH is the search path for looking for the resource file.  It
#         must be space delimited.
RES_PATH="${GPHOME}/bin/lib /etc/gpcheckos"

#TESTS2RUN is the list of tests to run.  The default runs the OS and network
#          configuration examinations and the patch examination."
TESTS2RUN=""

#QUIET_MODE forces almost all the messages to the log only even if they go
#           to the screen normally.
QUIET_MODE="false"

#TEMP_DIR is the location where temporary files are stored.
TEMP_DIR=/var/tmp/.gpcheckos/${RUN_NM}/${ITER}

usage()
{
#  clear
  echo
  echo "gpcheckos.bash version: ${VERSION}"
  echo
  echo
  echo "gpcheckos-sun.bash [ -a|--all"
  echo "		     -c|--config [path to gpcheckos-`uname -s`.res]"
  echo "                     -d|--interconnect"
  echo "                     -h|-?|--help" ]
  echo "                     -i|--iteration #"
  echo "		     -l|--logdir [path to store the log]"
  echo "                     -n|--network"
  echo "                     -o|--os"
  echo "                     -p|--patches"
  echo "                     -q|--quiet"
  echo "                     -r|--run [run name]"
  echo "                     -s|--storage"
  echo "                     -v|--version"
  echo
  echo " where:"
  echo
  echo "  -a runs all tests.  This switch overrides specification of -p, -o, -s"
  echo "     -n, or -d."
  echo
  echo "  -c [path to gpcheckos-`uname -s`.res] uses the path as the place to"
  echo "       find the resource file.  Defaults to ."
  echo "       Defaults to /tmp/[run name]_summary.log"
  echo
  echo "  -d runs an examination of the interconnect connectivity.  Assumes"
  echo "     that the standard hostnames (mdw, sdw1, sdw2-2, etc) apply."
  echo "     This switch is additive to -o -s, -n, and -p"
  echo
  echo "  -h shows this help."
  echo
  echo "  -i # uses the number in # as the current iteration # for"
  echo "       id purposes in the csv results.  Defaults to 0"
  echo
  echo "  -l [path to store the log] uses the path as the location for the"
  echo "       output files.  Defaults to /var/tmp"
  echo
  echo "  -n runs an examination of network configuration, NOT CONNECTIVITY"
  echo "     and notes deviations from the standard"
  echo "     This switch is additive to -o -s, -p, and -d"
  echo
  echo "  -o runs an examination of Solaris configuration options and"
  echo "     notes deviations from the standard."
  echo "     This switch is additive to -p -s, -n, and -d"
  echo
  echo "  -p runs an examination of the OS patches and notes exceptions."
  echo "     This switch is additive to -o -s, -n, and -d"
  echo
  echo "  -q suppresses most of the messages that normally show on screen."
  echo "     All messages continue to be captured to the log file.  This"
  echo "     option is intended to make this tool easier to run in gpssh."
  echo
  echo "  -r [run name] uses the string [run name] as the run identifier."
  echo "       Defaults to the script's pid"
  echo
  echo "  -s runs an examination of storage configuration (including SVM and"
  echo "     ZFS) and notes deviations from the standard."
  echo "     This switch is additive to -o -p, -n, and -d"
  echo
  echo "  -v shows the script version"
  echo
}


#parses command line arguments
parse_args()
{

  ARGS=`separate_args "${*}"`
  ARGS="${ARGS} -end"

  ARG_NM=""
  ARG_VALUE=""
  for i in ${ARGS}; do
    if [ "X${ARG_NM}" = "X" ]; then
      ARG_NM=${i}
    else
      case ${ARG_NM} in
        "-end")
          if [ ! "X${i}" = "X" ]; then
            usage
            exit 0
          fi
        ;;
         
        "-a"|"--all")
          TESTS2RUN=posnd
          ARG_NM=${i}
        ;; 

        "-c"|"--config")
          CONFIG_DIR=${i} 
          ARG_NM=""
        ;;

        "-d"|"--interconnect")
          echo ${TESTS2RUN} | grep d > /dev/null 2>&1
          rval=${?}
          if [ ! ${rval} -eq 0 ]; then
            TESTS2RUN=${TESTS2RUN}d
          fi
          ARG_NM=${i}
        ;; 

        "-h"|"-?"|"--help")
          usage
          exit 0
        ;;

        "-i"|"--iteration")
          ITER=${i} 
          ARG_NM=""
        ;;

        "-l"|"--logdir")
          LOG_DIR=${i}
          ARG_NM=""
        ;;

        "-n"|"--network")
          echo ${TESTS2RUN} | grep n > /dev/null 2>&1
          rval=${?}
          if [ ! ${rval} -eq 0 ]; then
            TESTS2RUN=${TESTS2RUN}n
          fi
          ARG_NM=${i}
        ;; 

        "-o"|"--os")
          echo ${TESTS2RUN} | grep o > /dev/null 2>&1
          rval=${?}
          if [ ! ${rval} -eq 0 ]; then
            TESTS2RUN=${TESTS2RUN}o
          fi
          ARG_NM=${i}
        ;; 

        "-p"|"--patches")
          echo ${TESTS2RUN} | grep p > /dev/null 2>&1
          rval=${?}
          if [ ! ${rval} -eq 0 ]; then
            TESTS2RUN=${TESTS2RUN}p
          fi
          ARG_NM=${i}
        ;; 

        "-q"|"--quiet")
          QUIET_MODE="true"
          ARG_NM=${i}
        ;;

        "-r"|"--run")
          RUN_NM=${i}
          ARG_NM=""
        ;;

        "-s"|"--storage")
          echo ${TESTS2RUN} | grep s > /dev/null 2>&1
          rval=${?}
          if [ ! ${rval} -eq 0 ]; then
            TESTS2RUN=${TESTS2RUN}s
          fi
          ARG_NM=${i}
        ;; 

        "-v"|"--version")
          SHOW_VERS=0
          ARG_NM=${i}
        ;;

        *)
          echo "ERROR: Unidentified switch ${ARG_NM}"
          exit 1
        ;;

      esac
    fi

  done

  case ${ARG_NM} in
    "-h"|"-?"|"--help")
      usage
      exit 0
    ;;
  esac

}

separate_args()
{
  ALL_ARGs="${*}"
  if [ ! "X${ALL_ARGs}" = "X" ]; then
    NEW_ARGS=""
    for i1 in `echo ${ALL_ARGs}`; do
      OPER1=`echo ${i1} | cut -c 1`
      OPER2=`echo ${i1} | cut -c 1-2`
      ARG_LEN=`printf "%s" "${i1}" | wc -m`
      if [ ${ARG_LEN} -gt 2 ] && [ "X${OPER1}" = "X-" ] && [ ! "X${OPER2}" = "X--" ]; then
        CNT=2
        while [ ${CNT} -le ${ARG_LEN} ]; do
          TMP_ARG=`echo ${i1} | cut -c ${CNT}`
          TMP_ARG="-"${TMP_ARG}
          NEW_ARGS=${NEW_ARGS}" "${TMP_ARG}
          CNT=`expr ${CNT} + 1`
        done
      else
        NEW_ARGS=${NEW_ARGS}" "${i1}
      fi
    done
  fi

  echo ${NEW_ARGS}
}

validate_args()
{
  
  RVAL=0

  #ITER
  #if ITER is non-numeric, complain and exit
  rval=`is_num ${ITER}`
  if [ ${rval} -eq 1 ]; then
    echo "ERROR:  iteration must be a number!"
    RVAL=1
  fi


  #RUN_NM
  #if there is already a fatal error, don't bother.
  if [ ${RVAL} -eq 0 ]; then
    #if run name is empty, complain and exit
    if [ "X${RUN_NM}" = "X" ]; then
      echo "ERROR: the run must have a name."
      RVAL=1
    fi
  fi
 

  #CONFIG_DIR
  #if there is already a fatal error, don't bother.
  if [ ${RVAL} -eq 0 ]; then
    #if config dir is not present, complain and exit.
    if [ -d ${CONFIG_DIR} ]; then
    
      #if resource file present in the current directory, 
      #use it instead and issue a warning
      if [ -f `pwd`/${RES_FILE_NM} ]; then
        CONFIG_DIR=`pwd`
      #otherwise, look for resource file in the provided location
      elif [ -f ${CONFIG_DIR}/${RES_FILE_NM} ]; then
        echo "INFO: Using ${CONFIG_DIR}/${RES_FILE_NM}"
      #if all else fails, look for the resource file in the defined search
      # path
      else
        CONFIG_DIR=""
        printf "\nChecking for resource file in non-standard path\n"
        for d in `echo ${RES_PATH}`; do
          if [ "X${CONFIG_DIR}" = "X" ]; then
            if [ -f ${d}/${RES_FILE_NM} ]; then
              echo "INFO: Using ${d}/${RES_FILE_NM}"
              CONFIG_DIR=${d}
            fi
          fi
        done
      fi
  
      #If no resource file found, complain and exit
      if [ "X${CONFIG_DIR}" = "X" ]; then
        echo "ERROR: Unable to find ${RES_FILE_NM}.  Please specify -c"
        RVAL=1
      fi
  
    else
      echo "ERROR: The config directory, ${CONFIG_DIR}, does not exist."
      RVAL=1
    fi
  fi
  

  #LOG_DIR
  #if there is already a fatal error, don't bother.
  if [ ${RVAL} -eq 0 ]; then
    #If LOG_DIR present, make sure we can write to it
    if [ -d ${LOG_DIR} ]; then
      touch ${LOG_DIR}/$$.test > /dev/null 2>&1
      rval=${?}
  
      #if able to write to LOG_DIR, clean out test file and make sub-folder
      if [ ${rval} -eq 0 ]; then
        rm ${LOG_DIR}/$$.test
        if [ ! "X${RUN_NM}" = "X" ]; then
          mkdir -p ${LOG_DIR}/${RUN_NM}/${ITER} > /dev/null 2>&1
        fi
   
      #if unable to write to LOG_DIR, complain and exit
      else
        echo "ERROR: Unable to write to log directory ${LOG_DIR}"
        RVAL=1
      fi

    #if LOG_DIR not present, create it
    else
      mkdir -p ${LOG_DIR} > /dev/null 2>&1
      rval=${?}
  
      #if able to make log directory, make sub-folder
      if [ ${rval} -eq 0 ]; then
        if [ ! "X${RUN_NM}" = "X" ]; then
          mkdir -p ${LOG_DIR}/${RUN_NM}/${ITER} > /dev/null 2>&1
        fi
  
      #if unable to make log directory, complain and exit
      else
        echo "ERROR: Unable to establish log directory ${LOG_DIR}."
        RVAL=1
      fi
    fi
  fi
    

  #TESTS2RUN
  #if there is already a fatal error, don't bother.
  if [ ${RVAL} -eq 0 ]; then
    #if there are no tests specified, reset the default
    if [ "X${TESTS2RUN}" = "X" ]; then
      TESTS2RUN=op
    #otherwise, make sure it has only supported values
    else
      LEN=`printf "%s" "${TESTS2RUN}" | wc -m`
      CNT=1
      while [ ${CNT} -le ${LEN} ]; do
        CUR_VAL=`echo ${TESTS2RUN} | cut -c ${CNT}`
        case ${CUR_VAL} in
          d|n|o|p|s)
          ;;

          *)
            RVAL=1
          ;;
        esac
        CNT=`expr ${CNT} + 1`
      done

      if [ ${RVAL} -eq 1 ]; then
        echo "ERROR: Invalid test specified."
      fi
    fi
  fi

  return ${RVAL}
}

#resolves times expressed as [min]m[sec] or [min]:[sec]
get_secs()
{
  #get time from first parameter
  C_VAL=${1}

  #get flag.  Must be either a "m" or ":"
  FLG=${2}

  if [ "X${FLG}" = "Xm" ]; then
    MIN=`echo ${C_VAL} | cut -d'm' -f1`
    SEC=`echo ${C_VAL} | cut -d'm' -f2`
  elif [ "X${FLG}" = "X:" ]; then
    MIN=`echo ${C_VAL} | cut -d':' -f1`
    SEC=`echo ${C_VAL} | cut -d':' -f2`
  else
    MIN=0
    SEC=0
  fi

  #convert minutes to seconds
  MIN=`echo "${MIN} * 60" | bc`

  #add sub-minute seconds to total
  C_VAL=`echo "scale=2;${MIN} + ${SEC}" | bc`

  #echo value
  echo ${C_VAL}
}

is_num()
{
  if [ "X${1}" = "X" ]; then
    RVAL=1
  else
    JUNK=`expr ${1} + 1 > /dev/null 2>&1`
    rval=${?}
    if [ ${rval} -eq 0 ]; then
      RVAL=0
    else
      RVAL=1
    fi
  fi

  echo ${RVAL}
}

#parse arguments
parse_args "${*}"

#set up the log locations
ld_parent=${LOG_DIR}/${RUN_NM}
LOG_DIR=${LOG_DIR}/${RUN_NM}/${ITER}
LOG_FILE=${LOG_DIR}/gpcheckos-`uname -s`.log
TEMP_DIR=/var/tmp/.gpcheckos/${RUN_NM}/${ITER}

# Clear out temp dir if it already exists
#if [ -d ${TEMP_DIR} ]; then
if [ -d /var/tmp/.gpcheckos ]; then
  rm -r /var/tmp/.gpcheckos/*
  rmdir /var/tmp/.gpcheckos
#  mv ${TEMP_DIR} ${TEMP_DIR}.$$
fi

mkdir -p ${TEMP_DIR} > /dev/null 2>&1
rval=${?}
if [ ! ${rval} -eq 0 ]; then
  echo "ERROR: Unable to create temp folder ${TEMP_DIR}!"
  exit 1
fi

#Check for the output directory.  copy it if found
if [ -d ${LOG_DIR} ]; then
  mv ${LOG_DIR} ${LOG_DIR}.$$
elif [ -f ${LOG_DIR} ]; then
  mv ${LOG_DIR} ${LOG_DIR}.$$
fi

#Check for output parent, copy it if found
if [ -d ${ld_parent} ]; then
  mv ${ld_parent} ${ld_parent}.$$
elif [ -f ${ld_parent} ]; then
  mv ${ld_parent} ${ld_parent}.$$
fi

#import the relevant test functions
if [ -f ${CONFIG_DIR}/gpcheckos-`uname -s`.lib ]; then
  . ${CONFIG_DIR}/gpcheckos-`uname -s`.lib
else
  echo "ERROR: Unable to find function library gpcheckos-`uname -s`.lib"
  echo "       in ${CONFIG_DIR}.  Please copy this file to the path."
  exit 1
fi

#validate arguments
validate_args
rval=${?}
if [ ! ${rval} -eq 0 ]; then
  echo "Unable to parse or validate arguments"
  exit 1
fi

#check that the user is root
check4root
rval=${?}
if [ ! ${rval} -eq 0 ]; then
  echo "ERROR: You must be root to run this script."
  exit 1
fi

#read the resource file
parse_res

lnout 0 0 ${LOG_FILE} ""
lnout 0 3 ${LOG_FILE} "Starting gpcheckos on `hostname`"
lnout 0 0 ${LOG_FILE} ""
lnout 1 0 ${LOG_FILE} "INFO: Using $CONFIG_DIR/${RES_FILE_NM}"
lnout 0 0 ${LOG_FILE} ""

#if the versions are to be shown, show them
if [ ${SHOW_VERS} -eq 0 ]; then
  versions ${VERSION}
  exit 0
fi

#show the discovered config summary
summary

#Run the os tests if selected
echo ${TESTS2RUN} | grep o > /dev/null 2>&1
rval=${?}
if [ ${rval} -eq 0 ]; then
  os_tests
fi

#Run the patch tests if selected
echo ${TESTS2RUN} | grep p > /dev/null 2>&1
rval=${?}
if [ ${rval} -eq 0 ]; then
  patches_tests
fi

#Run the storage tests if selected
echo ${TESTS2RUN} | grep s > /dev/null 2>&1
rval=${?}
if [ ${rval} -eq 0 ]; then
  storage_tests
fi

#Run the network tests if selected
echo ${TESTS2RUN} | grep n > /dev/null 2>&1
rval=${?}
if [ ${rval} -eq 0 ]; then
  network_tests
fi

#Run the network tests if selected
echo ${TESTS2RUN} | grep d > /dev/null 2>&1
rval=${?}
if [ ${rval} -eq 0 ]; then
  interconnect_tests
fi

lnout 0 0 ${LOG_FILE} ""
lnout 0 0 ${LOG_FILE} "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%"
lnout 0 0 ${LOG_FILE} ""
lnout 0 0 ${LOG_FILE} "All information can be found in ${LOG_DIR}"
lnout 0 3 ${LOG_FILE} "The log file is `hostname`:${LOG_FILE}"
lnout 0 0 ${LOG_FILE} ""
lnout 0 0 ${LOG_FILE} "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%"
lnout 0 0 ${LOG_FILE} ""
lnout 0 3 ${LOG_FILE} "gpcheckos on `hostname` is complete"
lnout 0 0 ${LOG_FILE} ""

# Clean up temp files
if [ -d /var/tmp/.gpcheckos ]; then
  rm -r /var/tmp/.gpcheckos/*
  rmdir /var/tmp/.gpcheckos
fi
