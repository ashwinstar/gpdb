#!/usr/bin/env python
from __future__ import generators
from xml.dom import minidom
from xml.dom import Node
import sys
from utils import *
import re
import gpcheckosVo
from gpcheckosVo import *
import types


def populateVo(node,pVo,config):

    #node has attributes and data
    if (node.attributes is not None) and (node.firstChild is not None): 
        for i in dir(pVo):
            #print "i is %s pvo.__getAttr__(i)) %s"% (i, type(pVo.__getAttr__(i)))
            if (type(pVo.__getAttr__(i)) ==  types.NoneType) and i != "__module__":
                #process attribute first 
                if node.hasAttribute(i): 
                    pVo.__setAttr__(i,node.getAttribute(i))
                #process node data
                #print "inside i is %s"%(i)
                if  (i == node.tagName):
                    #print "i is %s, firstChild.data is %s"%(i, node.firstChild.data)
                    pVo.__setAttr__( i, node.firstChild.data) 
                     
            elif (type(pVo.__getAttr__(i)) ==  types.ListType) and (i != "__module__"):
                cls=getattr(gpcheckosVo, (i[0].upper()+i[1:]).replace("List", "Vo") )
                clsVo = factory(cls)
                populateListVo(node,pVo,clsVo,i)
          

def populateListVo(node, pVo, listVo, vosName):
    #if (node.attributes is not None) or (node.firstChild is not None): 
    for i in dir(listVo):
        if type(listVo.__getAttr__(i)) in ( types.StringType, types.UnicodeType)  and i != "__module__":
            #process attribute first 
            if node.hasAttribute(i): 
                listVo.__setAttr__(i,node.getAttribute(i))
                if not listVoExist(pVo.__getAttr__(vosName), listVo):
                    pass
            #process node data
            if  (i == node.tagName):
                listVo.__setAttr__( i, node.firstChild.data) 
                if not listVoExist(pVo.__getAttr__(vosName), listVo): 
                    pVo.__getAttr__(vosName).append(listVo)
                    

def listVoExist( myList, listVo):
    # takes a list[] and value object as parmaeters and return true if listVo exist in the list
    #  
    result=0
    for v in myList:  
        for i in dir(listVo):
            if type(listVo.__getAttr__(i)) == types.StringType or \
               type(listVo.__getAttr__(i)) == types.UnicodeType and (i != "__module__") : 
                if (listVo.__getAttr__(i) != "") and ( v.__getAttr__(i) ==""):
                    v.__setAttr__(i, listVo.__getAttr__(i) ) 
                    result=1
    return result
           

def modifyList(listVo, vo):       
    for i in  dir(listVo):
        if   type(listVo.__getAttr__(i)) == types.StringType or \
             type(listVo.__getAttr__(i)) == types.UnicodeType and (i != "__module__") : 
            if (vo.__getAttr__(i) != "") and (listVo.__getAttr__(i) == ""):
               listVo.__setAttr__(i, vo.__getAttr__(i) )
                
    return listVo
