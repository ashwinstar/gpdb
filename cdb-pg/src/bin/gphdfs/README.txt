How to MAKE

The .class file is already part of the depot. So, there's no need to rebuilt
the .class unless the .java is changed. If the .java is changed and the .class
has to be recompile, you need to do the following:
  1. p4 edit hadoop_env.sh
  2. Update the JAVA_HOME, HADOOP_HOME and HADOOP_VERSION in hadoop_env.sh
  3. source hadoop_env.sh
  4. make gphdfs
  5. make install


HDFS Integrator

There are two ways to use the HDFS integrator. The first method is to create an external
table which executes the java program. The second method is to create an external table
which uses the "hdfs://" notation.

First, Java 1.6 and Hadoop must be installed.
Second, update the JAVA_HOME, HADOOP_HOME and HADOOP_VERSION in hadoop_env.sh.
Then, "make install" would compile and put the .class file and hadoop_env.shin the $GPHOME/bin dir for external table execution. Tar and re-ship the GP installation to all
segments so that the .class and hadoop_env.sh are located in the $GPHOME/bin in all
segments.


Method ONE: execute java program.

Use your own <host>, <port> and <hdfs_file_path> in the following CREATE TABLEstatement.

create external web table hdfs_read(c1 text)
  execute 'source $GPHOME/bin/hadoop_env.sh; java -classpath $CLASSPATH HDFSReader $GP_SEGMENT_ID $GP_SEGMENT_COUNT gphdfs://<host>:<port>/<hdfs_file_path>' on all
  format 'TEXT';

create writable external web table hdfs_write(c1 text)
  execute 'source $GPHOME/bin/hadoop_env.sh; java -classpath $CLASSPATH HDFSWRiter $GP_SEGMENT_ID $GP_XID gphdfs://<host>:<port>/<hdfs_file_path>'
  format 'TEXT';

Now, execute the external web table.
  select * from hdfs_read;
  insert into hdfs_write select * from some_Table;



Method TWO: hdfs://

create external table hdfs_read(c1 text)
location ('gphdfs://<hdfs_host>:<hdfs_port><hdfs_file_path>')
format 'TEXT';
