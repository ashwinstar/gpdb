import os, sys, datetime

gphome = os.environ.get('GPHOME')
if not gphome:
    sys.exit('GPHOME env not set')

sys.path.append(os.path.join(sys.path[0], 'lib'))
sys.path.append(os.path.join(gphome, 'bin', 'lib'))
import pg8000
import web
import session
import db

urls = ('/system', 'system', '/system/', 'system')

UNUSED = '''
def access(role = None):
    def decorator(func):
        def proxyfunc(self, *args, **kw):
            s = session.get(role)
            if None == s:
                return web.seeother('/error')
            kw['ses'] = s
            ret = func(self, *args, **kw)
            s.save();
            return ret;
        return proxyfunc
    return decorator
'''

render = web.template.render('./templates/', cache=False)
datadir = os.getenv('MASTER_DATA_DIRECTORY', None)
if not datadir:
    sys.exit('MASTER_DATA_DIRECTORY env not set')
if not os.path.isdir(datadir):
    sys.exit('MASTER_DATA_DIRECTORY "%s" is not valid' % datadir)



def error(msg):
    return render.error(web.ctx.fullpath, msg)

def timenow():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')


class system:
    #@access()
    def GET(self, ses=None):
        web.header('Content-Type', 'text/xml')
        i = web.input(start='last', end='', host='', interval='', sd='')
        i.start = i.start.lower()
        i.end = i.end.lower()
        res = None
        if i.start.lower() == 'last':
            res = db.metrics_now(i.host)
            if not res:
                return error('db.metrics_now failed')

        elif i.end.lower() == 'last':
            res = db.metrics_upto_now(i.host, i.start)
            if not res:
                return error('db.metrics_upto_now failed')

        else:
            res = db.metrics_history(i.host, i.start, i.end)
            if not res:
                return error('db.metrics_history failed')

        (tkeys, hkeys, ckeys, tab) = res
        if tkeys:
            stime = tkeys[0]; etime = tkeys[-1]
        else:
            stime = timenow(); etime = stime

        return render.system(stime, etime, tkeys, hkeys, ckeys, tab)

if __name__ == '__main__':
    web.webapi.internalerror = web.debugerror
    app = web.application(urls, globals(), web.reloader)
    app.run()

