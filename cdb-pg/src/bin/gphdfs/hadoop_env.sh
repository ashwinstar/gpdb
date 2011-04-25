export GP_JAVA_OPT=-Xmx10m
export PATH=$JAVA_HOME/bin:$PATH

CLASSPATH="$GPHOME"/bin
export HADOOP_COMMON_HOME="${HADOOP_HOME}"
# add libs to CLASSPATH
for f in $HADOOP_COMMON_HOME/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done

for f in $HADOOP_COMMON_HOME/lib/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done

if [ -d "$HADOOP_COMMON_HOME/build/ivy/lib/Hadoop-Common/common" ]; then
for f in $HADOOP_COMMON_HOME/build/ivy/lib/Hadoop-Common/common/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done
fi

if [ -d "$HADOOP_COMMON_HOME/build/ivy/lib/Hadoop-Hdfs/common" ]; then
for f in $HADOOP_COMMON_HOME/build/ivy/lib/Hadoop-Hdfs/common/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done
fi

if [ -d "$HADOOP_COMMON_HOME/build/ivy/lib/Hadoop/common" ]; then
for f in $HADOOP_COMMON_HOME/build/ivy/lib/Hadoop/common/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done
fi

for f in $HADOOP_COMMON_HOME/lib/jsp-2.1/*.jar; do
  CLASSPATH=${CLASSPATH}:$f;
done

export CLASSPATH=$CLASSPATH
