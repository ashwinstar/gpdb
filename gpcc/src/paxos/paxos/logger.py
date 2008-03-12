import time

disable = False

def setDisable(disabl):
    global disable
    disable = disabl

class Logger:

    def __init__(self,name):
	self.__name = ' '+name+' '

    def log(self,s):
	if disable:
	    return
	s = str(time.time())+self.__name+s
	if len(s)>=240:
	    s = s[:225]+" (%d more)"%(len(s)-225)
	t = ""
	for i in s:
	    t += i if i>=' ' and i<='~' else '.'
	print t
