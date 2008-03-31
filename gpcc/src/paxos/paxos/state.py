import utils

from stringchopper import StringChopper

E_NOENT = 1
E_ISDIR = 2
E_IFFAILED = 3
E_INVAL = 4

class StateException(Exception):

    def __init__(self,s):
	self.number = s
	if s==E_NOENT:
	    a = "No such file or directory"
	elif s==E_ISDIR:
	    a = "Is a directory"
	elif s==E_IFFAILED:
	    a = "If failed"
	elif s==E_INVAL:
	    a = "Invalid"
	else:
	    a = ""
	Exception.__init__(self,a)

class Value:

    def __init__(self,dn,string):
	self.dn = dn
	self.string = string

class Dir:

    def __init__(self,dn):
	self.__map = dict()
	self.dn = dn
	self.__cow = dn

    def list(self):
	return self.__map.keys()

    def get(self,name):
	try:
	    name = name.lstrip('/')
	    if not name:
		return self
	    i = name.find('/')
	    if i!=-1:
		o = self.__map[name[:i]]
		if not isinstance(o,Dir):
		    return None
		return o.get(name[i+1:])
	    return self.__map[name]
	except KeyError:
	    return None

    def __set(self,dn,cow,name,value):
	dir = self
	if dir.__map.get(name,None)!=value:
	    if dir.__cow<cow:
		dir = Dir(self.dn)
		dir.__cow = dn
		dir.__map.update(self.__map)
	    if value==None:
		dir.dn = dn
		dir.__map.pop(name)
	    else:
		if name not in dir.__map:
		    dir.dn = dn
		dir.__map[name] = value
	return dir

    def put(self,dn,cow,name,string):
	name = name.lstrip('/')
	if not name:
	    raise StateException(E_ISDIR)
	i = name.find('/')
	if i!=-1:
	    n = name[i+1:]
	    name = name[:i]
	    o = self.__map.get(name,Dir(dn))
	    if not isinstance(o,Dir):
		raise StateException(E_NOENT)
	    o = o.put(dn,cow,n,string)
	else:
	    if isinstance(self.__map.get(name,None),Dir):
		raise StateException(E_ISDIR)
	    o = Value(dn,string)
	return self.__set(dn,cow,name,o)

    def remove(self,dn,cow,name):
	try:
	    name = name.lstrip('/')
	    if not name:
		raise StateException(E_ISDIR)
	    i = name.find('/')
	    if i!=-1:
		n = name[i+1:]
		name = name[:i]
		o = self.__map[name]
		if not isinstance(o,Dir):
		    raise StateException(E_NOENT)
		o = o.remove(dn,cow,n)
		if not o.__map:
		    o = None
		return self.__set(dn,cow,name,o)
	    o = self.__map[name]
	    if isinstance(o,Dir):
		raise StateException(E_ISDIR)
	    return self.__set(dn,cow,name,None)
	except KeyError:
	    raise StateException(E_NOENT)

    def write(self,out):
	for name,o in sorted(self.__map.iteritems()):
	    if isinstance(o,Dir):
		utils.writeString(out,name+'/')
		out.write('\n')
		o.write(out)
	    else:
		utils.writeString(out,name)
		utils.writeLong(out,o.dn)
		utils.writeString(out,o.string)
		out.write('\n')
	utils.writeString(out,"")
	utils.writeLong(out,self.dn)
	out.write('\n')

    def read(self,ni):
	while 1:
	    name = utils.readString(ni)
	    if len(name)==0:
		break
	    if name.endswith('/'):
		d = Dir(0)
		d.read(ni)
		self.__map[name[:-1]] = d
	    else:
		dn = utils.readLong(ni)
		string = utils.readString(ni)
		self.__map[name] = Value(dn,string)
	self.dn = utils.readLong(ni)

class State:

    def __init__(self):
	self.__root = Dir(0)
	self.__cow = 0

    def getFixedRoot(self,dn):
	self.__cow = dn
	return self.__root

    def __get1(self,name):
	o = self.__root.get(name)
	if o==None:
	    raise StateException(E_NOENT)
	if isinstance(o,Dir):
	    sb = " ".join(sorted(o.list()))
	else:
	    sb = o.string
	return (o.dn,sb)

    def __commandGet(self,name,offset,size):
	o = self.__get1(name)
	return "%d %d "%(o[0],len(o[1]))+o[1][offset:offset+size]

    def __commandStat(self,name):
	o = self.__get1(name)
	return "%d %d"%(o[0],len(o[1]))

    def __commandRemove(self,name,dn):
	self.__root = self.__root.remove(dn,self.__cow,name)

    def __commandPut(self,name,dn,offset,string):
	if offset!=0:
	    o = self.__root.get(name)
	    if o==None:
		raise StateException(E_NOENT)
	    if isinstance(o,Dir):
		raise StateException(E_ISDIR)
	    if len(o.string)<offset:
		raise StateException(E_INVAL)
	    string = o.string[:offset]+string
	self.__root = self.__root.put(dn,self.__cow,name,string)

    def get(self,name):
	return self.__root.get(name)

    def list(self,name):
	a = self.__root.get(name)
	return a.list() if a!=None else set()

    def put(self,dn,name,string):
	self.__root = self.__root.put(dn,self.__cow,name,string)

    def remove(self,dn,name):
	self.__root = self.__root.remove(dn,self.__cow,name)

    def read(self,ni):
	self.__root = Dir(0)
	self.__root.read(ni)
	self.__cow = 0

    def execute(self,decree,dn,priestSet):
	if not decree:
	    return ""
	try:
	    sc = StringChopper(decree)
	    while 1:
		command = sc.nextToken()
		name = sc.nextToken()
		if command=="If":
		    l = int(sc.nextToken())
		    o = self.__root.get(name)
		    m = 0 if o==None else o.dn
		    if m!=l:
			raise StateException(E_IFFAILED)
		    continue
		if command=="Tag":
		    continue
		if command=="Put":
		    offset = int(sc.nextToken())
		    s = sc.remainingStuff()
		    self.__commandPut(name,dn,offset,s)
		    return "0"
		if command=="Get":
		    offset = int(sc.nextToken())
		    size = int(sc.nextToken())
		    return self.__commandGet(name,offset,size)
		if command=="Stat":
		    return self.__commandStat(name)
		if command=="Remove":
		    self.__commandRemove(name,dn)
		    return "0"
		if command=="GetPriestSet":
		    return "0 "+",".join(sorted(priestSet))
		raise StateException(E_INVAL)
	except StateException,e:
	    return "error %d %s"%(e.number,e.message)
	except Exception,e:
	    return "error %d %s"%(E_INVAL,e.message)
