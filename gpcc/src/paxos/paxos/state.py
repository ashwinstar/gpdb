import utils

from stringchopper import StringChopper

E_NOENT = 1
E_ISDIR = 2
E_IFFAILED = 3
E_INVAL = 4

class StateException(Exception):
    def __init__(self,s):
	Exception.__init__(self,str(s))

class Value:
    pass

class Dir:
    def __init__(self,dn):
	self.map = dict()
	self.dn = dn

class State:

    def __init__(self):
	self.__root = Dir(0)

    @staticmethod
    def __get(dir,name):
	if not name:
	    return dir
	i = name.find('/')
	if i!=-1:
	    o = State.__get(dir,name[:i])
	    if not isinstance(o,Dir):
		return None
	    return State.__get(o,name[i+1:])
	return dir.map[name] if name in dir.map else None

    @staticmethod
    def __put(dir,name,dn,string):
	if not name:
	    raise StateException(E_ISDIR)
	i = name.find('/')
	if i!=-1:
	    dirname = name[0:i]
	    o = State.__get(dir,dirname)
	    if o==None:
		dir.map[dirname] = o = Dir(dn)
		dir.dn = dn
	    if not isinstance(o,Dir):
		raise StateException(E_NOENT)
	    State.__put(o,name[i+1:],dn,string)
	else:
	    if name not in dir.map:
		dir.map[name] = Value()
		dir.dn = dn
	    o = dir.map[name]
	    if isinstance(o,Dir):
		raise StateException(E_ISDIR)
	    o.string = string
	    o.dn = dn

    @staticmethod
    def __remove(dir,name,dn):
	if not name:
	    raise StateException(E_ISDIR)
	i = name.find('/')
	if i!=-1:
	    dirname = name[:i]
	    o = State.__get(dir,dirname);
	    if not isinstance(o,Dir):
		raise StateException(E_NOENT)
	    State.__remove(o,name[i+1:],dn)
	    if not o.map and dirname:
		dir.map.pop(dirname)
		dir.dn = dn
	else:
	    if name not in dir.map:
		raise StateException(E_NOENT)
	    o = dir.map[name]
	    if isinstance(o,Dir):
		raise StateException(E_ISDIR)
	    dir.map.pop(name)
	    dir.dn = dn

    @staticmethod
    def __write(dir,out):
	for name,o in dir.map.iteritems():
	    if isinstance(o,Dir):
		utils.writeString(out,name+'/')
		out.write('\n')
		State.__write(o,out)
	    else:
		utils.writeString(out,name)
		utils.writeLong(out,o.dn)
		utils.writeString(out,o.string)
		out.write('\n')
	utils.writeString(out,"")
	utils.writeLong(out,dir.dn)
	out.write('\n')

    @staticmethod
    def __read(dir,ni):
	while 1:
	    name = utils.readString(ni)
	    if len(name)==0:
		break
	    if name.endswith('/'):
		d = Dir(0)
		State.__read(d,ni)
		dir.map[name[:-1]] = d
	    else:
		v = Value()
		v.dn = utils.readLong(ni)
		v.string = utils.readString(ni)
		dir.map[name] = v
	dir.dn = utils.readLong(ni)

    def __get1(self,name):
	o = State.__get(self.__root,name)
	if o==None:
	    raise StateException(E_NOENT)
	if isinstance(o,Dir):
	    sb = ""
	    for s in sorted(o.map.keys()):
		if sb:
		    sb += ' '
		sb += s
	else:
	    sb = o.string
	return (o.dn,sb)

    def __commandGet(self,name,offset,size):
	o = self.__get1(name)
	return "%d %d "%(o[0],len(o[1]))+o[1][offset:offset+size]

    def __commandStat(self,name):
	o = self.__get1(name)
	return "%d %d"%(o[0],len(o[1]))

    def __commandPut(self,name,dn,offset,string):
	if offset!=0:
	    o = State.__get(self.__root,name)
	    if o==None:
		raise StateException(E_NOENT)
	    if isinstance(o,Dir):
		raise StateException(E_ISDIR)
	    if len(o.string)<offset:
		raise StateException(E_INVAL)
	    string = o.string[:offset]+string
	State.__put(self.__root,name,dn,string)

    def get(self,name):
	return State.__get(self.__root,name)

    def list(self,name):
	a = State.__get(self.__root,name)
	return a.map.keys() if a!=None else set()

    def put(self,name,dn,string):
	return State.__put(self.__root,name,dn,string)

    def remove(self,name,dn):
	State.__remove(self.__root,name,dn)

    def write(self,out):
	State.__write(self.__root,out);
 
    def read(self,ni):
	self.__root.map.clear()
	State.__read(self.__root,ni)

    def execute(self,decree,dn):
	if not decree:
	    return ""
	try:
	    sc = StringChopper(decree)
	    while 1:
		command = sc.nextToken()
		name = sc.nextToken()
		if command=="If":
		    l = int(sc.nextToken())
		    o = State.__get(self.__root,name)
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
		    State.__remove(self.__root,name,dn)
		    return "0"
		raise StateException(E_INVAL)
	except StateException,e:
	    return "error "+e.message
