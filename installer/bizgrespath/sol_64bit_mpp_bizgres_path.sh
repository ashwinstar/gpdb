BIZHOME=/usr/local/bizgres
MPPHOME=$BIZHOME/MPP_RELEASE_VERSION
JAVA_HOME=/usr/local/bizgres/jdk1.5.0_05
PATH=$MPPHOME/bin:$MPPHOME/gpMgmt/bin:$BIZHOME/client/loader/bin:$JAVA_HOME/bin/amd64:$BIZHOME/gnu/bin:$PATH
LD_LIBRARY_PATH=$MPPHOME/lib
MANPATH=$BIZHOME/doc:$MANPATH

export BIZHOME
export MPPHOME
export JAVA_HOME
export PATH
export LD_LIBRARY_PATH
export MANPATH
