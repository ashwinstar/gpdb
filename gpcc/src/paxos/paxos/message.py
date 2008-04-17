PAXOS_PROTOCOL_VERSION = 0

class Maker:

    def __init__(self,command):
	self.__sb = [command]
	self.__sb.append("version=%d"%PAXOS_PROTOCOL_VERSION)

    def send(self,port,to):
	a = self.toString()
	if type(to)==str:
	    port.send(to,a)
	else:
	    for s in to:
		port.send(s,a)

    def toString(self):
	return " ".join(self.__sb)

    def addPriestId(self,priestId):
	self.__sb.append("priestId="+priestId)

    def addDecreeNumber(self,dn):
	self.__sb.append("dn="+str(dn))

    def addBallotNumber(self,bn):
	self.__sb.append("bn="+str(bn))

    def addDecree(self,decree):
	self.__sb.append("decree="+self.__mangle(decree))

    def addResult(self,result):
	self.__sb.append("result="+self.__mangle(result))

    @staticmethod
    def __mangle(s):
	sb = []
	for c in s:
	    if c==' ' or c=='!':
		sb.append('!')
		c = chr(ord(c)+1)
	    sb.append(c)
	return "".join(sb)

class Message:

    def __init__(self,messageAndFrom):
	self.fro = messageAndFrom[1]
	st = messageAndFrom[0].split(' ')
	self.command = st.pop(0)
	self.priestId = None
	self.dn = None
	self.bn = None
	self.decree = None
	self.result = None
	self.version = None
	for s in st:
	    i = s.index('=')
	    value = s[i+1:]
	    s = s[:i]
	    if s=="version":
		self.version = int(value)
	    elif s=="priestId":
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
	sb = []
	i = 0
	j = len(s)
	while i<j:
	    c = s[i]
	    i += 1
	    if c=='!':
		c = chr(ord(s[i])-1)
		i += 1
	    sb.append(c)
	return "".join(sb)

def getMessage(messageAndFrom):
    if messageAndFrom[0]:
	m = Message(messageAndFrom)
	if m.version==PAXOS_PROTOCOL_VERSION:
	    return m
