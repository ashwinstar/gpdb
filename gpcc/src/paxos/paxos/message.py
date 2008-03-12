class Maker:

    def __init__(self,command):
	self.__sb = command

    def send(self,port,to):
	if type(to)==str:
	    port.send(to,self.__sb)
	else:
	    for s in to:
		port.send(s,self.__sb)

    def toString(self):
	return self.__sb

    def addPriestId(self,priestId):
	self.__sb += " priestId="+priestId

    def addDecreeNumber(self,dn):
	self.__sb += " dn="+str(dn)

    def addBallotNumber(self,bn):
	self.__sb += " bn="+str(bn)

    def addDecree(self,decree):
	self.__sb += " decree="+self.__mangle(decree)

    def addResult(self,result):
	self.__sb += " result="+self.__mangle(result)

    @staticmethod
    def __mangle(s):
	sb = ""
	for c in s:
	    if c==' ' or c=='!':
		sb += '!'
		c = chr(ord(c)+1)
	    sb += c
	return sb

class Message:

    def __init__(self,messag,fro):
	self.fro = fro
	st = messag.split(' ')
	self.command = st.pop(0)
	for s in st:
	    i = s.index('=')
	    value = s[i+1:]
	    s = s[:i]
	    if s=="priestId":
		self.priestId = value
	    elif s=="dn":
		self.dn = int(value)
	    elif s=="bn":
		self.bn = int(value)
	    elif s=="decree":
		self.decree = self.__unmangle(value)
	    elif s=="result":
		self.result = self.__unmangle(value)
	    else:
		assert 0

    @staticmethod
    def __unmangle(s):
	sb = ""
	escape = False
	for c in s:
	    if escape:
		sb += chr(ord(c)-1)
		escape = False
	    elif c=='!':
		escape = True
	    else:
		sb += c
	return sb
