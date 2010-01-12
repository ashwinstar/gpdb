#!/usr/bin/env python
import sys
import utils
import types

class OsParmVo():
    def __init__(self):
        self.sysctlConfList=[]
        self.limitsConfList=[]
    def __getAttr__(self, attr):
        return getattr(self,attr)

    def __setAttr__(self, attr, value):
        setattr(self, attr,value)

    def getSysctlConfVo(self,param):
        vo=None
        for i in self.sysctlConfList:
            if i.param == param:
                vo = i
                break
        return vo
    def getLimitsConfVo(self,param):
        vo=None
        for i in self.sysctlConfList:
            if i.param == param:
                vo = i
                break
        return vo
    def addSysctlConfVo(self,vo):
        self.sysctlConfList.append(vo)
    def addLimitsConfVo(self,vo):
        self.sysctlConfList.append(vo)
    def addBlockDevVo(self,vo):
        self.blockDev.append(vo)

class SysctlConfVo:
    def __init__(self):
        self.param=""
        self.value=""
    def __getAttr__(self, attr):
        return getattr(self,attr)
    def __setAttr__(self, attr, value):
        setattr(self, attr,value)
class LimitsConfVo:
    def __init__(self):
        self.limit=""
        self.hardValue=""
        self.softValue=""
    def __getAttr__(self, attr):
        return getattr(self,attr)
    def __setAttr__(self, attr, value):
        setattr(self, attr,value)
