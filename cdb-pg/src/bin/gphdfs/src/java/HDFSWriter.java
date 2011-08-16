package com.emc.greenplum.gpdb;

import com.emc.greenplum.gpdb.hadoop.io.GPDBWritable;

import java.net.URI;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.fs.FileStatus;
import org.apache.hadoop.io.*;
import org.apache.hadoop.util.*;
import org.apache.hadoop.mapreduce.*;
import org.apache.hadoop.io.compress.*;
import org.apache.hadoop.mapreduce.lib.output.*;

import java.io.*;

class HDFSWriter
{
	protected int     segid;
    protected String  xid;
    protected boolean isTextFormat;
	protected URI     fsURI;
	protected String  outputpath;
    protected Configuration conf;
	protected TaskAttemptContext fakeTaskCtx;

	protected boolean isCompressed = false;
	protected String  compressType = null;
	protected String  compressCodec = null;
	

	private boolean isdebug=false;
	private InputStream instream;
    
	/**
	 * Constructor - parse the input args
	 */
	public HDFSWriter(String[] args) throws Exception {
		/* Env var check */
		String hdVer = System.getenv("GP_HADOOP_VERSION");
		if (hdVer == null)
			throw new Exception("Missing GP_HADOOP_VERSION env var");

	    /* The following input argments are required:
	     * arg[0] : local segment id
         * arg[1] : xid
	     * arg[2] : format: TEXT or GPDBWritable
	     * arg[3] : gphdfs://host:port/file_path?<query>
	     * query contains:
	     *  compress: true or false
	     * 	compression_type (for gpdbwritable only): BLOCK and RECORD (gpdbwritable)
	     *  codec (for gpdbwritable only): codec class name
	     */
	    if (args.length != 4)
	        throw new Exception("Wrong number of argument");

		segid        = Integer.parseInt(args[0]);
		xid          = args[1]; 
		isTextFormat = (args[2].equals("TEXT"));
		URI inputURI = new URI(args[3]);
		
		/**
		 * Initialize conf, a fake ctx and conf derived var
		 */
	    conf        = new Configuration();
	    fakeTaskCtx = new TaskAttemptContext(conf, new TaskAttemptID());
	    isCompressed = FileOutputFormat.getCompressOutput(fakeTaskCtx);
	    compressType = conf.get("mapred.output.compression.type",
	    		SequenceFile.CompressionType.RECORD.toString());
	    compressCodec = conf.get("mapred.output.compression.codec");
		
		/* Parse the URI and reconstruct the destination URI
		 * 1. scheme could be hdfs or maprfs
		 * 2. get the path
		 * 3. parse the compression type and codec from the URI query
		 */
	    String uriScheme = "hdfs";
	    if (hdVer.startsWith("EE")) {
	    	uriScheme = "maprfs";
	    	conf.set("fs.maprfs.impl", "com.mapr.fs.MapRFileSystem");
	    }
	    fsURI = new URI(uriScheme, null, inputURI.getHost(), inputURI.getPort(), null, null, null);
	    conf.set("fs.default.name", fsURI.toString());
	    
	    outputpath = inputURI.getPath();
	    
	    String[] kv_pair = inputURI.getQuery().split("&");
	    for(int i=0; i<kv_pair.length; i++) {
	    	String[] kv = kv_pair[i].split("=");
	    	String key = kv[0];
	    	String val = kv[1];
	    	
	    	if (key.equals("compress"))	    	isCompressed  = val.equals("true");
	    	if (key.equals("compression_type"))	compressType  = val;
	    	if (key.equals("codec"))    		compressCodec = val;
	    }
	    
	    

	    if (isdebug) {
	    	byte[] bytes = (new String("sdfsdfs\n123123123\naaaaa")).getBytes();
	    	instream = new ByteArrayInputStream(bytes);
	    }
	    else
	    	instream = System.in;
	}


	/** 
	 * Either TEXT or GPDBWritable. Call the appropiate write method
	 */
	public void run() throws Exception {
		if (isTextFormat)
			writeTextFormat();
		else
			writeGPDBWritableFormat();
	}
	
	/**
	 * Helper routine to get the compression codec class
	 */
	protected Class<? extends CompressionCodec>
	getCodecClass(String name, Class<? extends CompressionCodec> defaultValue) {
		Class<? extends CompressionCodec> codecClass = defaultValue;
	    if (name != null) {
	    	try {
	    		codecClass = conf.getClassByName(name).asSubclass(CompressionCodec.class);
	    	} catch (ClassNotFoundException e) {
	    		throw new IllegalArgumentException("Compression codec " + name + " was not found.", e);
	    	}
	    }
	    return codecClass;
	}

	
	/**
	 * Write as SequenceFile <LongWritable, GPDBWritable>
	 */
	protected void writeGPDBWritableFormat() throws Exception {
		RecordWriter<LongWritable, GPDBWritable> rw = getGPDBRecordWriter();
		LongWritable segIDWritable = new LongWritable(segid);
		
		DataInputStream dis = new DataInputStream(instream);
		
		// Read from Std-in to construct "value"
		try {
			while(true) {
				GPDBWritable gw = new GPDBWritable();
				gw.readFields(dis);
				rw.write(segIDWritable, gw);
			}
		} catch (EOFException e) {}
		
		rw.close(fakeTaskCtx);
	}
	
	/**
	 * Write as TextOutputFormat
	 */
	protected void writeTextFormat() throws Exception {
		// construct the output stream
		DataOutputStream dos;
		String     filename = outputpath+"/"+segid+"_"+xid;
	    FileSystem fs       = FileSystem.get(fsURI, conf);

	    if (!isCompressed) {
		    Path file = new Path(filename);
	    	dos = fs.create(file, false);
	    }
	    else {
	        CompressionCodec codec = 
	        	(CompressionCodec) ReflectionUtils.newInstance(
	        			getCodecClass(compressCodec, GzipCodec.class),
	        			conf);
	        String extension = codec.getDefaultExtension();
	        
	        Path file = new Path(filename+extension);
	        FSDataOutputStream fileOut = fs.create(file, false);
	    	dos = new DataOutputStream(codec.createOutputStream(fileOut));
	    }
		
	    // read bytes from std-in and write it to the outputstream
	    byte[] buf = new byte[1024];
	    int  read;
	    while((read=instream.read(buf)) > 0)
	      dos.write(buf,0,read);

	    dos.close();
	}
		
	/**
	 * Helper function to construct the SequenceFile writer
	 */
	protected SequenceFile.Writer getSequenceWriterWriter() throws Exception {
	    CompressionCodec codec = null;
	    SequenceFile.CompressionType compressionType = SequenceFile.CompressionType.NONE;
	    if (isCompressed) {
	      // find the kind of compression to do and the right codec
	      compressionType = SequenceFile.CompressionType.valueOf(compressType);
	      codec = (CompressionCodec) ReflectionUtils.newInstance(
	    		  	getCodecClass(compressCodec, DefaultCodec.class),
	    		  	conf);

	    }
	    // get the path of the  output file 
	    Path file = new Path(outputpath+"/"+segid+"_"+xid);
	    FileSystem fs = file.getFileSystem(conf);
	    return SequenceFile.createWriter(fs, conf, file,
	             LongWritable.class,
	             GPDBWritable.class,
	             compressionType,
	             codec,
	             fakeTaskCtx);
	}
	
	protected RecordWriter<LongWritable, GPDBWritable> getGPDBRecordWriter() throws Exception {
		 final SequenceFile.Writer out = getSequenceWriterWriter();

		 return new RecordWriter<LongWritable, GPDBWritable>() {

		     public void write(LongWritable key, GPDBWritable value)
		       throws IOException {

		       out.append(key, value);
		     }

		     public void close(TaskAttemptContext context) throws IOException { 
		       out.close();
		     }
		   };
	}


	public static void main(String[] args) throws Exception
	{
		HDFSWriter hw = new HDFSWriter(args);
		hw.run();
	} 	

}
