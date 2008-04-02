import time

class Logger:

    def __init__(self,name):
	self.__name = ' '+name+' '
	self.__disable = False

    def setDisableLog(self,disabl):
	self.__disable = disabl

    def log(self,s):
	if self.__disable:
	    return
	s = str(time.time())+self.__name+s
	if len(s)>=240:
	    s = s[:225]+" (%d more)"%(len(s)-225)
	t = []
	for i in s:
	    t.append(i>=' ' and i<='~' and i or '.')
	print "".join(t)
