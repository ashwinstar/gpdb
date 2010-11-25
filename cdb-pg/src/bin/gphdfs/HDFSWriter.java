import java.net.URI;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.fs.FSDataOutputStream;
import org.apache.hadoop.fs.FileStatus;
import java.io.EOFException;

class HDFSWriter
{
  private static final int  BUFSIZE = 1000;
  
  public static void main(String[] args) throws Exception
  {
    /* 3 input argments are required:
     * arg[0]: segid
     * arg[1]: xid
     * arg[2]: host/port/path in the format of gphdfs://<host>:<port>/path
    */
    if (args.length < 3)
      throw new Exception("Wrong number of argument");

    int    segid = Integer.parseInt(args[0]);
    String xid   = args[1]; 
    String hostport_path[] = args[2].substring(9).split("/", 2);
    String hostport        = "hdfs://"+hostport_path[0];
    String pathstr         = "/"+hostport_path[1]+"/"+segid+"_"+xid;

    URI                hdfsURI  = URI.create(hostport);
    Configuration      conf     = new Configuration();
    FileSystem         fs       = FileSystem.get(hdfsURI, conf);
    Path               filepath = new Path(pathstr);
    FSDataOutputStream outFile  = fs.create(filepath, true);

    byte buf[] = new byte[BUFSIZE];
    int  read;

    while((read=System.in.read(buf)) > 0)
      outFile.write(buf,0,read);

    outFile.close();
  }
}
