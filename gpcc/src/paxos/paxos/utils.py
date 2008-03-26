def stringToAddresses(m,s):
    return set([m.normalize(i.strip()) for i in s.split(',')])

def skipSpaces(di):
    while 1:
	i = di.read(1)
	if len(i)==0:
	    raise EOFError()
	if i not in ' \n\r\t':
	    return i

def readString(di):
    if skipSpaces(di)!='"':
	assert 0
    sb = []
    escape = False
    while 1:
	i = di.read(1)
	if len(i)==0:
	    raise EOFError()
	if escape:
	    sb.append(i)
	    escape = False
	elif i=='\\':
	    escape = True
	elif i=='"':
	    return "".join(sb)
	else:
	    sb.append(i)

def readLong(di):
    sb = [skipSpaces(di)]
    while 1:
	i = di.read(1)
	if len(i)==0:
	    raise EOFError()
	if i>='0' and i<='9':
	    sb.append(i)
	elif i not in ' \n\r\t':
	    assert 0
	else:
	    return int("".join(sb))

def writeString(od,s):
    od.write('"')
    for c in s:
	if c=='\\' or c=='"':
	    od.write('\\')
	od.write(c)
    od.write('"')

def writeLong(od,l):
    od.write(str(l)+' ')
