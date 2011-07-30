package org.gpdb;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

import org.apache.hadoop.io.LongWritable;
import org.apache.hadoop.io.Writable;
import org.apache.hadoop.mapreduce.InputFormat;
import org.apache.hadoop.mapreduce.InputSplit;
import org.apache.hadoop.mapreduce.Job;
import org.apache.hadoop.mapreduce.JobContext;
import org.apache.hadoop.mapreduce.MRJobConfig;
import org.apache.hadoop.mapreduce.RecordReader;
import org.apache.hadoop.mapreduce.TaskAttemptContext;
import org.apache.hadoop.util.ReflectionUtils;
import org.apache.hadoop.classification.InterfaceAudience;
import org.apache.hadoop.classification.InterfaceStability;
import org.apache.hadoop.conf.Configurable;
import org.apache.hadoop.conf.Configuration;

import org.apache.hadoop.mapreduce.lib.db.*;

/**
 * A InputFormat that reads input data from an SQL query.
 * <p>
 * GPDBInputFormat emits a null key and GPDBWritables as value. 
 * 
 * The SQL query, and input class can be using one of the two 
 * setInput methods.
 */
@InterfaceAudience.Public
@InterfaceStability.Stable
public class GPDBInputFormat
    extends InputFormat<LongWritable, GPDBWritable> implements Configurable {

  private String dbProductName = "GPDB";
 
  private Connection connection;

  private GPDBConfiguration dbConf;
  private Configuration     jobConf;

  /** {@inheritDoc} */
  public void setConf(Configuration conf) {
	  jobConf = conf;
    dbConf = new GPDBConfiguration(conf);
  }

  public Configuration getConf() {
    return jobConf;
  }
  
  public GPDBConfiguration getDBConf() {
    return dbConf;
  }

  public Connection getConnection() {
    try {
      if (null == this.connection) {
        // The connection was closed; reinstantiate it.
        this.connection = dbConf.getConnection(false);
        this.connection.setAutoCommit(false);
        this.connection.setTransactionIsolation(
            Connection.TRANSACTION_SERIALIZABLE);
      }
    } catch (Exception e) {
      throw new RuntimeException(e);
    }
    return connection;
  }

  public String getDBProductName() {
    return dbProductName;
  }

  /** {@inheritDoc} */
  @SuppressWarnings("unchecked")
  public RecordReader<LongWritable, GPDBWritable> createRecordReader(InputSplit split,
      TaskAttemptContext context) throws IOException, InterruptedException {  
    return new GPDBRecordReader();
  }
  
  /**
   * getGPDBSplit
   * 
   * return the input split if it's coming from a query or table.
   */
  protected List<InputSplit> getGPDBSplits(JobContext job) throws IOException {
	  try {
	      // List all the GPDB segment here if they're in the Hadoop cluster.
		  Connection gpdb_conn = dbConf.getConnection(false);
		  String selectHostAddr = "select hostname, content from gp_segment_configuration where content>=0 and role='p'";
	      PreparedStatement ps = gpdb_conn.prepareStatement(selectHostAddr);
	      ResultSet rs = ps.executeQuery();
	      List<InputSplit> splits = new ArrayList<InputSplit>();
	      
	      while(rs.next()){
	          GPDBRecordReader.GPDBInputSplit split = new GPDBRecordReader.GPDBInputSplit();
	          split.location    = new String[1];
	          split.location[0] = rs.getString(1);
	          split.sid         = rs.getInt(2);
	          splits.add(split);
	      }
	      return splits;
	  } catch (Exception e) {throw new IOException(e.toString());}
  }
  
  /**
   * getHDFSSplit
   * 
   * return the input split if the input is actually HDFS files in
   * GPDB binary format.
   * 
   * TODO: raise an exception for now..
   */
  protected List<InputSplit> getHDFSSplits(JobContext job) throws IOException {
	  throw new IOException("File based InputSplit is not supported yet");
  }

  /** {@inheritDoc} */
  public List<InputSplit> getSplits(JobContext job) throws IOException {
	  String fileloc = jobConf.get(GPDBConfiguration.GPDB_INPUT_FILELOC);
	  if (fileloc == null)
		  return getGPDBSplits(job);
	  else
		  return getHDFSSplits(job);
  }

}
