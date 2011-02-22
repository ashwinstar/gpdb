import java.net.URI;
import org.apache.hadoop.fs.FileSystem;
import org.apache.hadoop.conf.Configuration;
import org.apache.hadoop.fs.Path;
import org.apache.hadoop.fs.FSDataInputStream;
import org.apache.hadoop.fs.FileStatus;
import java.io.EOFException;

class HDFSReader
{
  private static final int  BUFSIZE = 4000;
  
  public static void main(String[] args) throws Exception
  {
    /* 3 input argments are required:
     * arg[0] : local segment id
     * arg[1] : total number of segments
     * arg[2+]: gphdfs://host:port/file_path
     */
    if (args.length < 3)
      throw new Exception("Wrong number of argument");

    int segid    = Integer.parseInt(args[0]);
    int totalseg = Integer.parseInt(args[1]); 

    if (segid == -1) segid = 0;

    /* For each file in the list, return the portion that this particular
     * segment is responsible, which is between startpos and endpos, by
     * printing it to stdout.
     */
    for(int i=2; i<args.length; i++)
    {
      String hostport_path[] = args[i].substring(9).split("/", 2);
      String hostport        = "hdfs://"+hostport_path[0];
      String pathstr         = "/"+hostport_path[1];

      URI               hdfsURI  = URI.create(hostport);
      Configuration     conf     = new Configuration();
      FileSystem        fs       = FileSystem.get(hdfsURI, conf);

      Path              filepath = new Path(pathstr);
      FSDataInputStream inFile   = fs.open(filepath);
      FileStatus        fstatus  = fs.getFileStatus(filepath);
      byte[]            inbuf    = new byte[BUFSIZE];
      int               readlen  = -1;

      long startpos   = fstatus.getLen()*segid/totalseg;    //inclusive
      long endpos     = fstatus.getLen()*(segid+1)/totalseg;//exclusive
      long remain_len = endpos-startpos;

      /* Find the starting point
       * When reading the first chunk (segid==0), just start from zero.
       * For others, go pass the first \n before start reading.
       */
      inFile.seek(startpos);
      if (segid>0)
      {
        byte newline = 0;
        while(remain_len > 0 && newline != '\n')
        {
        	newline=inFile.readByte();
        	remain_len--;
        }
        if (remain_len==0)
        {
          fs.close();
          return;
        }
      }

      // Read until we reach the end pos
      int read = 0;
      while(remain_len>0)
      {
        if (remain_len > BUFSIZE)
          read = inFile.read(inbuf, 0, BUFSIZE);
        else
          read = inFile.read(inbuf, 0, (int)remain_len);
        remain_len -= read;
        System.out.write(inbuf, 0, read);
      }

      // Now, read until we find the next \n
      byte newline;
      try
      {
        while((newline=inFile.readByte()) != '\n')
          System.out.write(newline);
        System.out.println();
      } catch (EOFException e) {;}
      fs.close();
    }
  }
}
