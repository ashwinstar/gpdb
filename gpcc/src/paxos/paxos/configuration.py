class Configuration:

    def __init__(self,s):
	self.priests = ""
	self.host = ""
	self.port = 0
	self.__loadConfig(s)

    def __loadConfig(self,config):
	st = [s.strip() for s in config.split('\n')]
	while st:
	    line = []
	    s = st.pop()
	    while s.endswith("\\") and st:
		line.append(s[:-1])
		s = st.pop()
	    line.append(s)
	    s = " ".join(line).strip()
	    if not s or s.startswith('#'):
		continue
	    value = ""
	    i = s.find('=')
	    if i!=-1:
		value = s[i+1:].strip()
		s = s[:i].strip()
	    self.__gotLine(s,value);

    def __gotLine(self,key,value):
	if key=="priests":
	    self.priests = value
	elif key=="host":
	    self.host = value
	elif key=="port":
	    self.port = int(value)
	else:
	    raise Exception("Got bad configuration "+key+'='+value);
