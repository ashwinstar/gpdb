import utils

from stringchopper import StringChopper

E_NOENT = 1
E_ISDIR = 2
E_IFFAILED = 3
E_INVAL = 4
E_EXIST = 5

def normalizeFilename(filename):
    return "/".join(filter(None,filename.split("/")))
def dirname(filename):
    return "/".join(filter(None,filename.split("/"))[:-1])

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
	elif s==E_EXIST:
	    a = "File Exists"
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
	    if not name:
		return self
	    i = name.find('/')
	    if i!=-1:
		o = self.__map[name[:i]]
		if not isinstance(o,Dir):
		    return
		return o.get(name[i+1:])
	    return self.__map[name]
	except KeyError:
	    pass

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

    def __put1(self,dn,cow,name,oo):
	if not name:
	    raise StateException(E_ISDIR)
	i = name.find('/')
	if i!=-1:
	    n = name[i+1:]
	    name = name[:i]
	    o = self.__map.get(name,None)
	    if not isinstance(o,Dir):
		raise StateException(E_NOENT)
	    oo = o.__put1(dn,cow,n,oo)
	return self.__set(dn,cow,name,oo)

    def move(self,dn,cow,fname,tname):
	if not fname or not tname or fname==tname:
	    raise StateException(E_INVAL)
	if tname.startswith(fname+'/'):
	    raise StateException(E_INVAL)
	o = self.get(fname)
	if not o:
	    raise StateException(E_NOENT)
	dir = self.__put1(dn,cow,tname,o)
	if not fname.startswith(tname+'/'):
	    dir = dir.__put1(dn,cow,fname,None)
	return dir

    def put(self,dn,cow,name,string):
	o = self.get(name)
	if isinstance(o,Dir):
	    raise StateException(E_ISDIR)
	return self.__put1(dn,cow,name,Value(dn,string))

    def remove(self,dn,cow,name):
	o = self.get(name)
	if o==None:
	    raise StateException(E_NOENT)
	if isinstance(o,Dir):
	    raise StateException(E_ISDIR)
	return self.__put1(dn,cow,name,None)

    def mkdir(self,dn,cow,name):
	o = self.get(name)
	if o!=None:
	    raise StateException(E_EXIST)
	return self.__put1(dn,cow,name,Dir(dn))

    def rmdir(self,dn,cow,name):
	o = self.get(name)
	if o==None:
	    raise StateException(E_NOENT)
	if not isinstance(o,Dir):
	    raise StateException(E_INVAL)
	return self.__put1(dn,cow,name,None)

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

    def __dirdn(self,name):
	o = self.__root.get(dirname(name))
	return o and o.dn or 0

    def __get1(self,name):
	o = self.__root.get(name)
	if o==None:
	    return
	if isinstance(o,Dir):
	    sb = " ".join(sorted(o.list()))
	else:
	    sb = o.string
	return (o.dn,sb)

    def __commandGetPriestSet(self,priestSet):
	return "0 "+",".join(sorted(priestSet))

    def __commandGet(self,name,offset,size):
	o = self.__get1(name)
	if o==None:
	    raise StateException(E_NOENT)
	return "%d %d %d "%(o[0],len(o[1]),self.__dirdn(name))+o[1][offset:offset+size]

    def __commandStat(self,name):
	o = self.__get1(name)
	if o==None:
	    o = (0,"")
	return "%d %d %d"%(o[0],len(o[1]),self.__dirdn(name))

    def __commandPut(self,dn,name,offset,string):
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

    def mkdir(self,dn,name):
	self.__root = self.__root.mkdir(dn,self.__cow,name)

    def rmdir(self,dn,name):
	self.__root = self.__root.rmdir(dn,self.__cow,name)

    def move(self,dn,fname,tname):
	self.__root = self.__root.move(dn,self.__cow,fname,tname)

    def list(self,name):
	a = self.__root.get(name)
	if a!=None:
	    return a.list()
	return set()

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
		name = normalizeFilename(sc.nextToken())
		if command=="If":
		    l = int(sc.nextToken())
		    o = self.__root.get(name)
		    if o==None:
			m = 0
		    else:
			m = o.dn
		    if m!=l:
			raise StateException(E_IFFAILED)
		    continue
		if command=="Tag":
		    continue
		if command=="Move":
		    toname = normalizeFilename(sc.nextToken())
		    self.move(dn,name,toname)
		    return "0"
		if command=="Mkdir":
		    self.mkdir(dn,name)
		    return "0"
		if command=="Rmdir":
		    self.rmdir(dn,name)
		    return "0"
		if command=="Put":
		    offset = int(sc.nextToken())
		    s = sc.remainingStuff()
		    self.__commandPut(dn,name,offset,s)
		    return "0"
		if command=="Get":
		    offset = int(sc.nextToken())
		    size = int(sc.nextToken())
		    return self.__commandGet(name,offset,size)
		if command=="Stat":
		    return self.__commandStat(name)
		if command=="Remove":
		    self.remove(dn,name)
		    return "0"
		if command=="GetPriestSet":
		    return self.__commandGetPriestSet(priestSet)
		raise StateException(E_INVAL)
	except StateException,e:
	    return "error %d %s"%(e.number," ".join(map(str,e.args)))
	except Exception,e:
	    return "error %d %s"%(E_INVAL," ".join(map(str,e.args)))
