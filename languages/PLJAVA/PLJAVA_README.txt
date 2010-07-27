************************************************************************
ABOUT PL/JAVA IN GREENPLUM DATABASE
************************************************************************

PL/JAVA is a PostGreSQL language extension that allows you to write custom 
database functions in the JAVA programming language.

In order to use PL/JAVA in Greenplum Database, you must install JDK 1.6
on every host in your Greenplum Database array.



***************************************************
INSTALLING AND ENABLING PL/JAVA IN GREENPLUM DATABASE
***************************************************

1. To use PL/JAVA you must have a JDK 1.6 installation on all
   Greenplum hosts (master and segments). Make sure the JAVA_HOME 
   environment variable is set in the environment of the 'gpadmin'
   user on all Greenplum hosts.        

2. Make sure the $JAVA_HOME environment variable is set in the 
   environment of the 'gpadmin' user on all Greenplum hosts:

    $ gpssh -f <all_hosts_file> -e "echo $JAVA_HOME"

3. 

4. Enable PL/JAVA in your database(s). Enabling PL/JAVA in the template1 
   database will also enable PL/JAVA in any new databases you create.

    $ psql template1 -c "CREATE LANGUAGE pljava;"
   
    $ psql <database_name> -c "CREATE LANGUAGE pljava;" 

NOTE: If the CREATE LANGUAGE command does not succeed because a
      library (.so file) could not be found, you may need to update
      your $LD_LIBRARY_PATH environment variable on all Greenplum
      hosts so that the JDK libraries are searched first. For example, 
      add the following line to the 'gpadmin' .bashrc on all hosts
      after your setting for JAVA_HOME:
      
       export LD_LIBRARY_PATH=$JAVA_HOME/lib:$LD_LIBRARY_PATH

      Then restart your Greenplum Database system:

       gpstop -r
 
   

