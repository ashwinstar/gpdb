package com.emc.greenplum.gpdb;

import com.emc.greenplum.gpdb.hadoop.io.GPDBWritable;

import java.net.URI;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FileStatus;

import org.apache.hadoop.mapreduce.*;
import org.apache.hadoop.mapreduce.lib.input.*;
import org.apache.hadoop.io.*;

import java.io.EOFException;
import java.io.DataOutputStream;

import java.util.*;

class HDFSReader
{
	private static final int  BUFSIZE = 4000;
	protected int     segid;
	protected int     totalseg; 
	protected boolean isTextFormat;
	protected String  hostport;
	protected String  inputpath;
    protected Configuration conf;
    protected ArrayList<InputSplit> segSplits;

	/**
	 * Constructor - parse the input args
	 */
	public HDFSReader(String[] args) throws Exception {
	    /* 4 input argments are required:
	     * arg[0] : local segment id
	     * arg[1] : total number of segments
	     * arg[2] : format: TEXT or GPDBWritable
	     * arg[3+]: gphdfs://host:port/file_path
	     */
	    if (args.length < 4)
	        throw new Exception("Wrong number of argument");

		segid        = Integer.parseInt(args[0]);
		totalseg     = Integer.parseInt(args[1]); 
		isTextFormat = (args[2].equals("TEXT"));
		
		String hdVer = System.getenv("GP_HADOOP_VERSION");
		if (hdVer == null)
			throw new Exception("Missing GP_HADOOP_VERSION env var");
	    conf             = new Configuration();
	    String uriPrefix = "hdfs://";
	    boolean isEE     = hdVer.startsWith("EE");
	    if (isEE) {
	      uriPrefix = "maprfs:///";
	      conf.set("fs.maprfs.impl", "com.mapr.fs.MapRFileSystem");
	    }
	    String hostport_path[] = args[3].substring(9).split("/", 2);
	    String hostport  = uriPrefix+hostport_path[0];
	    conf.set("fs.default.name", hostport);
	    
	    inputpath = "/"+hostport_path[1];
	}
	
	/** 
	 * Either TEXT or GPDBWritable. Call the appropiate read method
	 */
	public void run() throws Exception {
		assignSplits();

		if (isTextFormat)
			readTextFormat();
		else
			readGPDBWritableFormat();
	}
	
	/**
	 * Read the input as a TextInputFormat
	 * and write it to Std Out
	 */
	protected void readTextFormat() throws Exception {
		// Text uses LineRecordReader
		LineRecordReader lrr = new LineRecordReader();
		TaskAttemptContext fakeTaskCtx = new TaskAttemptContext(conf, new TaskAttemptID());
		
		// For each split, use the record reader to read the stuff
		for(int i=0; i<segSplits.size(); i++) {
			lrr.initialize(segSplits.get(i), fakeTaskCtx);
			while(lrr.nextKeyValue()) {
				Text val = lrr.getCurrentValue();
				System.out.println(val.toString());
				System.out.flush();
			}
		}
	}
	
	/**
	 * Read the input as GPDBWritable from SequenceFile
	 * and write it to Std Out
	 */
	protected void readGPDBWritableFormat() throws Exception {
		// GPDBWritable uses SequenceFileRecordReader
		SequenceFileRecordReader<IntWritable, GPDBWritable> sfrr = new SequenceFileRecordReader();
		TaskAttemptContext fakeTaskCtx = new TaskAttemptContext(conf, new TaskAttemptID());
		
		// For each split, use the record reader to read the stuff
		DataOutputStream dos = new DataOutputStream(System.out);
		for(int i=0; i<segSplits.size(); i++) {
			sfrr.initialize(segSplits.get(i), fakeTaskCtx);
			while(sfrr.nextKeyValue()) {
				GPDBWritable val = sfrr.getCurrentValue();
				val.write(dos);
				dos.flush();
			}
		}
	}

	/**
	 * Create a list of input splits from the input path
	 * and then assign some splits to our segment
	 */
	protected void assignSplits() throws Exception {
		// Create a input split list for the input path
		Job jobCtx = new Job(conf);
		FileInputFormat.setInputPaths(jobCtx, new Path(inputpath));
		FileInputFormat fformat = (isTextFormat)? new TextInputFormat() : new SequenceFileInputFormat(); 
		List<InputSplit> splits = fformat.getSplits(jobCtx);

		// Decide which split to operate on
		segSplits = new ArrayList(splits.size()/totalseg);
		if (segid==0)
			segSplits.add(splits.get(0));
		for(int i=1; i<splits.size(); i++) {
			if (i%totalseg == segid)
				segSplits.add(splits.get(i));
		}
	}
	
	public static void main(String[] args) throws Exception
	{
		HDFSReader hr = new HDFSReader(args);
		hr.run();
	} 	
}
