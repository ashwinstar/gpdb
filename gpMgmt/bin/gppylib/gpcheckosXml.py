#!/usr/bin/env python
from __future__ import generators
from xml.dom import minidom
from xml.dom import Node
#from xml.dom.ext.reader.Sax2 import FromXmlStream
import sys
from stringUtil import *
import re
import gpcheckosVo
from gpcheckosVo import *
from gpcheckosDa import *

config = ""
if "None" == `os.environ.get("GPCHECKOS_XML")` :
    GPHOME=os.environ.get("GPHOME") 
    config=os.path.join(GPHOME,'docs/cli_help/gpcheckos.xml')
else:
    config = "/gpcheckos/conf/gpcheckos.xml"

_debug = 0
def debug(varname, o):
    if _debug == 1:
        print "Debug: %s -> %s" %(varname, o)

class GpXml:
    class __GpXml:
        
        def _open(self):
            """ file descriptor """
            fdesc = utils.openAnything(config)
            xmldoc = minidom.parse(fdesc).documentElement
            fdesc.close()
            return xmldoc
 

        def loadbackup(self, backup):
            """ get the backup section from gp.xml. This is a one-off for the backup
	 	before loadElementByTagName was written, need to refactor later	
	    """
        
            dom = self._open()
            nodeBackup = dom.getElementsByTagName(backup) 
            for node in nodeBackup:
                srcDir = node.getElementsByTagName("srcDir")
            return nodeBackup


        def getXmlConfig(self, moduleConfig, lineConfig):
            """ get the backup section from gp.xml. This is a one-off for the backup
	    """

            nodes=self.loadbackup(moduleConfig)
            t=[]
            for node in nodes:
                lcNodes = node.getElementsByTagName(lineConfig) 
                for x in lcNodes:
                    if x.firstChild != None:
                        t.append(x.firstChild.data)
                        self.getNodeAttrs(x)
            return t

################## official methods section

	def loadElementByTagName(self, elementName):
            """  loadElementByTagName method takes element Name and xml attribute name
            """
            dom = self._open()
            debug("elementName is ", elementName)
            elements= dom.getElementsByTagName(elementName)
            debug("elements", elements)
            return elements

	def loadRootNode(self, elementName):
            """  loadRootNode method takes element Name and xml attribute name
            """
            doc = self._open()
            rootNode=doc.documentElement
            return rootNode
  
      
        
        def findNode(self,nodeName):
	    t=nodeName[0].upper() + nodeName[1:] + "Vo"
            cls=getattr(gpcheckosVo, nodeName[0].upper() + nodeName[1:] +"Vo") 
            vo=factory(cls)

            parentNode=self.loadElementByTagName(nodeName)
            for c in parentNode:
                for x in docIter(c):
                    if x.nodeType == x.ELEMENT_NODE:
                        populateVo(x,vo,config) 
                        #vo=populateVo(x,vo) 
            return vo
    

 
    ''' singleton check '''
    instance = None
    def __init__(self):
        if GpXml.instance is None:
            # create and remember instace
            GpXml.instance = GpXml.__GpXml()
        self.__dict__['_GpXml_instance'] = GpXml.instance

    def __getattr__(self, attr):
        return getattr(self.instance, attr)
    
    def __setattr__(self, attr, value):
        return setattr(self.instance, attr, value)
       
