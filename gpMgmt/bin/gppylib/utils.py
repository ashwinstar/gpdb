import shutil, filecmp,re
import os, fcntl, select, getpass
from subprocess import *
from sys import *
from xml.dom import minidom
from xml.dom import Node


_debug=0
#############
class ParseError(Exception):
    def __init__(self,parseType):
        self.msg = ('%s parsing error'%(parseType))
    def __str__(self):
        return self.msg

#############
class RangeError(Exception):
    def __init__(self, value1, value2):
        self.msg = ('%s must be less then %s' % (value1, value2))
    def __str__(self):
        return self.msg

#############
def createFromSingleHostFile(inputFile):
    """TODO: """
    rows=[]
    f = open(inputFile, 'r')
    for line in f:
      rows.append(parseSingleFile(line))
    
    return rows


#############
def toNonNoneString(value) :
    if value is None:
        return ""
    return str(value)

#
# if value is None then an exception is raised
#
# otherwise value is returned
#
def checkNotNone(label, value):
    if value is None:
        raise Exception( label + " is None")
    return value

#
# value should be non-None
#
def checkIsInt(label, value):
    if type(value) != type(0):
        raise Exception( label + " is not an integer type" )

def isNone( value):
    isN=False
    if value is None:
        isN=True 
    elif value =="":
        isN= True
    return isN 

def readAllLinesFromFile(fileName):
    res = []
    with open(fileName) as f:
        for line in f:
            res.append(line)
    return res

def writeLinesToFile(fileName, lines):
    with open(fileName, 'w') as f:
        for line in lines:
            f.write(line)
            f.write('\n')

#############
def parseSingleFile(line):
    ph=None 
    if re.search(r"^#", line):
        #skip it, it's a comment
        pass
    else:
        ph=line.rstrip("\n").rstrip()  
    return ph

def openAnything(source):            
    """URI, filename, or string --> stream

    This function lets you define parsers that take any input source
    (URL, pathname to local or network file, or actual data as a string)
    and deal with it in a uniform manner.  Returned object is guaranteed
    to have all the basic stdio read methods (read, readline, readlines).
    Just .close() the object when you're done with it.
    
    Examples:
    >>> from xml.dom import minidom
    >>> sock = openAnything("http://localhost/kant.xml")
    >>> doc = minidom.parse(sock)
    >>> sock.close()
    >>> sock = openAnything("c:\\inetpub\\wwwroot\\kant.xml")
    >>> doc = minidom.parse(sock)
    >>> sock.close()
    >>> sock = openAnything("<ref id='conjunction'><text>and</text><text>or</text></ref>")
    >>> doc = minidom.parse(sock)
    >>> sock.close()
    """
    if hasattr(source, "read"):
        return source

    if source == '-':
        import sys
        return sys.stdin

    # try to open with urllib (if source is http, ftp, or file URL)
    import urllib                         
    try:                                  
        return urllib.urlopen(source)     
    except (IOError, OSError):            
        pass                              
    
    # try to open with native open function (if source is pathname)
    try:                                  
        return open(source)               
    except Exception, e: 
        print ("Exception occurred opening file %s Error: %s"  % (source, str(e)))                             
        
    
    # treat source as string
    import StringIO                       
    return StringIO.StringIO(str(source)) 
def getOs():
    dist=None
    fdesc = None
    RHId = "/etc/redhat-release"
    SuSEId = "/etc/SuSE-release"
    try: 
        fdesc = open(RHId)
        for line in fdesc: 
            line = line.rstrip()   
            if re.match('CentOS', line):
                dist = 'CentOS' 
            if re.match('Red Hat', line):
                dist = 'CentOS' 
    except IOError:
        pass
    finally:
        if fdesc :
            fdesc.close()
    try: 
        fdesc = open(SuSEId)
        for line in fdesc: 
            line = line.rstrip()   
            if re.match('SUSE', line):
                dist = 'SuSE' 
    except IOError:
        pass
    finally:
        if fdesc : 
            fdesc.close()
    return dist
def factory(aClass, *args):
    return apply(aClass,args)

def addDicts(a,b):
    c = dict(a)
    c.update(b)
    return c

def joinPath(a,b,parm=""):
    c=a+parm+b 
    return c

def debug(varname, o):
    if _debug == 1:
        print "Debug: %s -> %s" %(varname, o)

def loadXmlElement(config,elementName):
    fdesc = openAnything(config)
    xmldoc = minidom.parse(fdesc).documentElement
    fdesc.close()
    elements=xmldoc.getElementsByTagName(elementName) 
    return elements

def docIter(node):
    """
        Iterates over each node in document order, returning each in turn
    """
    #Document order returns the current node,
    #then each of its children in turn
    yield node
    if node.nodeType == Node.ELEMENT_NODE:
        #Attributes are stored in a dictionary and
        #have no set order. The values() call
        #gets a list of actual attribute node objects
        #from the dictionary
        for attr in node.attributes.values():
            yield attr
    for child in node.childNodes:
        #Create a generator for each child,
        #Over which to iterate
        for cn in docIter(child):
            yield cn
    return

def makeNonBlocking(fd):
    fl = fcntl.fcntl(fd, os.F_GETFL)
    try:
        fcntl.fcntl(fd, os.F_SETFL, fl | os.O_NDELAY)
    except AttributeError:
        fcntl.fcntl(fd, os.F_SETFL, fl | os.FNDELAY)

def getCommandOutput2(command):
    child = popen2.Popen3(command, 1) # Capture stdout and stderr from command
    child.tochild.close( )             # don't need to write to child's stdin
    outfile = child.fromchild
    outfd = outfile.fileno( )
    errfile = child.childerr
    errfd = errfile.fileno( )
    makeNonBlocking(outfd)            # Don't deadlock! Make fd's nonblocking.
    makeNonBlocking(errfd)
    outdata, errdata = [  ], [  ]
    outeof = erreof = False
    while True:
        to_check = [outfd]*(not outeof) + [errfd]*(not erreof)
        ready = select.select(to_check, [  ], [  ]) # Wait for input
        if outfd in ready[0]:
            outchunk = outfile.read( )
            if outchunk == '':
                outeof = True
            else:
                outdata.append(outchunk)
        if errfd in ready[0]:
            errchunk = errfile.read( )
            if errchunk == '':
                erreof = True
            else:
                errdata.append(errchunk)
        if outeof and erreof:
            break
        select.select([  ],[  ],[  ],.1) # Allow a little time for buffers to fill
    err = child.wait( )
    if err != 0:
        raise RuntimeError, '%r failed with exit code %d\n%s' % (
            command, err, ''.join(errdata))
    return ''.join(outdata)

def getCommandOutput(command):
    child = os.popen(command)
    data = child.read( )
    err = child.close( ) 
    #if err :
    #    raise RuntimeError, '%r failed with exit code %d' % (command, err)
    return ''.join(data)


def touchFile(fileName):
    if os.path.exists(fileName):
            os.remove(fileName)
    fi=open(fileName,'w')
    fi.close()

def deleteBlock(fileName,beginPattern, endPattern):
    #httpdConfFile="/etc/httpd/conf/httpd.conf"
    fileNameTmp= fileName +".tmp"
    if beginPattern is None :
        beginPattern = '#gp begin'

    if endPattern is None :
        endPattern = '#gp end'

    beginLineNo = 0
    endLineNo = 0
    lineNo =1

    #remove existing gp existing entry
    if os.path.isfile(fileName):
        try:
            fdesc = open(fileName)
            lines = fdesc.readlines()
            fdesc.close()
            for line in lines:
                line = line.rstrip()
                if re.match(beginPattern, line):
                    beginLineNo = lineNo
                    #print line
                    #print beginLineNo
                if re.match(endPattern, line) and (beginLineNo != 0):
                    endLineNo = lineNo
                    #print endLineNo
                lineNo += 1
                #print lines[beginLineNo-1:endLineNo]
                del lines[beginLineNo-1:endLineNo]
                fdesc = open(fileNameTmp,"w")
                fdesc.writelines(lines)
                fdesc.close()
                os.rename(fileNameTmp,fileName)
        except IOError:
            print("IOERROR", IOError)
            sys.exit()
    else:
        print "***********%s  file does not exits"%(fileName)

def make_inf_hosts(hp, hstart, hend, istart, iend, hf=None):
    hfArr = []
    inf_hosts=[]
    if None != hf:
        hfArr=hf.split('-')
    print hfArr 
    for h in range(int(hstart), int(hend)+1):
        host = '%s%d' % (hp, h)
        for i in range(int(istart), int(iend)+1):
            if i != 0 :
                inf_hosts.append('%s-%s' % (host, i))
            else:
                inf_hosts.append('%s' % (host))
    return inf_hosts

def copyFile(srcDir,srcFile, destDir, destFile):
    result=""
    filePath=os.path.join(srcDir, srcFile)
    destPath=os.path.join(destDir,destFile)
    if not os.path.exists(destDir):
        os.makedirs(destDir)
    try:
        if os.path.isfile(filePath):
            #debug("filePath" , filePath)
            #debug("destPath" , destPath)
            pipe=os.popen("/bin/cp -avf  " +filePath +" "+destPath)
            result=pipe.read().strip()
            #debug ("result",result)
        else:
            print "no such file or directory " + filePath
    except OSError:
        print ("OS Error occurred")
    return result


def sortedDictByKey(di):
    return  [ (k,di[k]) for k in sorted(di.keys())]

