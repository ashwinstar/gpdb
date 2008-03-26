class StringChopper:

    def __init__(self,s):
	self.__s = s
	self.__i = 0

    def nextToken(self):
	j = self.__i
	while self.__i<len(self.__s):
	    if self.__s[self.__i]==' ':
		self.__i += 1
		return self.__s[j:self.__i-1]
	    self.__i += 1
	return self.__s[j:]

    def remainingStuff(self):
	return self.__s[self.__i:]
