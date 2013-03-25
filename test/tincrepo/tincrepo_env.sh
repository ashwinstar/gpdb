# if TINCHOME not set fail

if [ -z $TINCHOME ] ; then
   echo "TINCHOME not set"
   return 1
fi

TINCREPOHOME=`cd $(dirname ${BASH_SOURCE[0]}); pwd`
export PYTHONPATH=$TINCHOME:$TINCHOME/ext:$TINCREPOHOME:$PYTHONPATH
