from logger import Logger

class Role(Logger):

    def __init__(self,port):
	self.port = port
	self.address = port.getAddress()
	name = str(self.__class__)
	name = name[name.index('.')+1:]
	if name.endswith("'>"):
	    name = name[:-2]
	Logger.__init__(self,name+'-'+self.address)

    def run(self):
	self.log("starting")
	try:
	    self.work()
	except:
	    self.log("got except")
	    self.log("done")
	    raise
	self.log("done")
