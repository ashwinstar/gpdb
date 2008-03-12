from logger import Logger

class Role(Logger):

    def __init__(self,port):
	self.port = port
	self.address = port.getAddress()
	name = str(self.__class__)
	name = name[name.index('.')+1:]
	Logger.__init__(self,name+'-'+self.address)

    def __call__(self):
	self.log("starting")
	try:
	    while 1:
		self.work()
	except:
	    self.log("got except")
	    raise
	finally:
	    self.log("done")
