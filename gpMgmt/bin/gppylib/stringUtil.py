#!/usr/bin/env python
from utils import *

"""
Usage: stringUtil [Options]

Description: insert a string into file based on matching string  
             if -s or --searchstring is not specified, the string 
             is inserted at the end of file

Options:
  -h, --help       	Show this help
  -f, --file       	file to modify
  -s, --searchstring    matching String
  -a, --appendstring    String to append
  -d, --delete          String to delete

"""
import os,sys,re
from optparse import Option, OptionGroup, OptionParser, OptionValueError, SUPPRESS_USAGE

def appendString(stringToAppend,file=None,stringToSearch=None,endOfLine=None):
    # insert a  strings in a text file
    if file == None:
        print "%s does not exist"%(file)
        exit       
    back = os.path.splitext(file)[0] + ".bak"
    temp = os.path.splitext(file)[0] + ".tmp"
    try:
        # remove old temp file, if any
        os.remove(temp)
    except os.error:
        pass
    if stringToSearch == None:
        fi = open(file,'a')
        try:
            fi.writelines(stringToAppend+'\n')
        finally:
            fi.close()
    else:
        fi = open(file)
        fo = open(temp, "w")
        try:
            for s in fi.readlines():
                #fo.write(string.replace(s, search_for, replace_with))
                if re.search(stringToSearch,s):
                    if endOfLine == 1: 
                        fo.write(s.strip("\n") + "   " + stringToAppend + '\n')
                    else:
                        fo.write(s)
                        fo.write(stringToAppend +'\n' )
                else:
                    fo.write(s)
                
        finally:
            fi.close()
            fo.close()
    try:
        # remove old backup file, if any
        os.remove(back)
    except os.error:
        pass

    # rename original to backup...
    if stringToSearch is not None:
        os.rename(file, back)

    # ...and temporary to original
    if stringToSearch is not None:
        os.rename(temp, file)

def deleteString(stringToDelete, directory, file):
    filePath=os.path.join(directory, file)
    destPath=os.path.join("/tmp",".fstab")

    fi=open(filePath)
    fo=open(destPath,"w")
    try:
        for s in fi.readlines():
            if re.search(stringToDelete,s):
                print s
            else:
                fo.write(s)
    finally:
        fi.close()
        fo.close()
    copyFile('/tmp', '.fstab', directory,file)
    os.remove("/tmp/.fstab" )

def deleteSubString(subStringToDelete,directory,file):
    filePath=os.path.join(directory, file)
    destPath=os.path.join("/tmp", file + ".bak")
    fi=open(filePath)
    fo=open(destPath,"w")
    try:
        for s in fi.readlines():
            if re.search(subStringToDelete,s):
                s = s.replace(subStringToDelete,"")
            fo.write(s)
    finally:
        fi.close()
        fo.close()
    copyFile('/tmp', file+".bak", directory,file)
    os.remove("/tmp/"+file+".bak" )

def usage():
    print "Usage: stringUtil [Options]"

    print "Description: insert or delete  a string into/from file based on matching string"
    print "  if -s or --searchstring is not specified, the string"
    print "  is inserted at the end of file"
    print " " 
    print "  Options:"
    print " 	-h, --help            Show this help"
    print " 	-d, --delete         remove entire string from a file "
    print " 	-u, --subdelete      remove substring from string  "    
    print " 	-p, --path            /directory/to/file"
    print " 	-f, --file            file to modify"
    print "  	-s, --searchstring    matching String"
    print "  	-a, --appendstring    String to append"
    print "example:"
    print "stringUtil -d -p /root/.ssh -f known_hosts -s node006"

def parseargs():
    pass


def main(argv):
    numArgs = len(argv)
    directory=None
    file=None
    delete=None
    subdelete=None
    stringToSearch=None
    stringToAppend=None

    if numArgs < 1 or numArgs > 9 :
        usage()
        print "************* Too few or many arguments ******************"
        sys.exit()
    try:
        opts,args = getopt.getopt(argv, "hdf:s:a:p:u",["help","delete","file","searchstring=","appendstring=","path=","subdelete"])                                       
    except Exception, e:
        usage()
        sys.exit(2)
    if len(opts) == 0:
       usage()
       sys.exit()
    for opt, arg in opts:
        if opt in ("-h", "--help"):
           usage()
           sys.exit()
        elif opt in ("-f", "--file"):
           file = arg
        elif opt in ("-s", "--searchstring"):
           stringToSearch=arg
        elif opt in ("-a", "--appendstring"):
           stringToAppend = arg
        elif opt in ("-p", "--path"):
           directory = arg
        elif opt in ("-d", "--delete"):
           delete = True
        elif opt in ("-u", "--subdelete"):
           subdelete = True
        else:
           usage()
           sys.exit()

    if (file == None ): 
           usage()
           print "no file is specified, use -f <file name>"
           sys.exit()
    if (delete==True):
        if (directory == None):
            usage()
            print "no path is specified, use -p </path/to/file>"
            sys.exit()
            
        if (stringToSearch == None):
            usage()
  	    print "no string specified, use -s <string>"
            sys.exit()
        deleteString( stringToSearch, directory,file)
    elif (subdelete==True):
        if (directory == None):
            usage()
            print "no path is specified, use -p </path/to/file>"
            sys.exit()
            
        if (stringToSearch == None):
            usage()
  	    print "no string specified, use -s <string>"
            sys.exit()
        deleteSubString( stringToSearch, directory,file)
    else:
        if (appendString == None ): 
            usage()
            print "no string to append  specified, use -a <string>"
            sys.exit()
        appendString(stringToAppend, file,stringToSearch)

if __name__ == "__main__":
    main(sys.argv[1:])
