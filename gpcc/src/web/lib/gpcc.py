import os, sys, urllib, httplib, socket

def hitMA(host, port, cmd, params):
    params = urllib.urlencode(params)
    f = None
    try:
        f = httplib.HTTPConnection('%s:%d' % (host, port))
        f.request('GET', '%s?%s' % (cmd, params))
        response = f.getresponse()
	return response
    except socket.error, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    except httplib.HTTPException, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    except Exception, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    finally:
        if f: f.close()
    return None  


def hitMAbyPost(host, port, cmd, params, data):
    params = urllib.urlencode(params)
    f = None
    res = ''
    try:
        f = httplib.HTTPConnection('%s:%d' % (host, port))
        f.request('POST', '%s?%s' % (cmd, params), data)
        response = f.getresponse()
	return response
    except socket.error, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    except httplib.HTTPException, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    except Exception, msg:
    	sys.stderr.write('requesting http://%s:%d%s failed. %s\n' % (host, port, cmd, msg))
        return None
    finally:
        if f: f.close()
    return None  
