import gzip
import os
import threading

from role import Role

class StateSaver(Role,threading.Thread):

    def __init__(self,priest,filename,root,dn):
	Role.__init__(self,priest.port)
	threading.Thread.__init__(self)
	self.__priest = priest
	self.__root = root
	self.filename = filename
	self.dn = dn

    def work(self):
	dos = gzip.GzipFile(self.filename,'wb')
	self.__root.write(dos)
	dos.flush()
	self.log("decreeNumber=%d crc=%d"%(self.dn,dos.crc))
	os.fsync(dos)
	dos.close()
