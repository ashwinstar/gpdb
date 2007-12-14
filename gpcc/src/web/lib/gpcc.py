import os, sys, urllib, httplib, socket

def getMA(ma_list, dead_ma):
    new_ma = None
    for addr in ma_list:
        if addr not in dead_ma:
            return addr
    return None
            
    
def hitMA(addr, cmd, params):
    params = urllib.urlencode(params)
    f = None
    try:
        f = httplib.HTTPConnection(addr)
        f.request('GET', '%s?%s' % (cmd, params))
        response = f.getresponse()
	return response
    except socket.error, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    except httplib.HTTPException, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    except Exception, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    finally:
        if f: f.close()
    return None  


def hitMAbyPost(addr, cmd, params, data):
    params = urllib.urlencode(params)
    f = None
    res = ''
    try:
        f = httplib.HTTPConnection(addr)
        f.request('POST', '%s?%s' % (cmd, params), data)
        response = f.getresponse()
	return response
    except socket.error, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    except httplib.HTTPException, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    except Exception, msg:
    	sys.stderr.write('requesting http://%s%s failed. %s\n' % (addr, cmd, msg))
        return None
    finally:
        if f: f.close()
    return None  
