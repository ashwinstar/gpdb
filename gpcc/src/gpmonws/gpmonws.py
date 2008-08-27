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





class system:
    #@access()
    def GET(self, ses=None):
        i = web.input(start='', end='', host='', interval='', sd='')
        if i.start.lower() == 'last':
            dgen = db.metrics_now()
            hosts = [h for h in dgen]
            if hosts:
                ctime = hosts[0]['ctime']
            else: 
                ctime = datetime.datetime.now().replace(microsecond=0).isoformat(' ')

            return render.system(ctime, hosts)

        sys.stderr.write("system\n")
        sys.stderr.write('start=%s\n' % str(i.start))
        sys.stderr.write('host=%s\n' % str(i.host))
        sys.stderr.write('interval=%s\n' % str(i.interval))
        return 'system'

if __name__ == '__main__':
    web.webapi.internalerror = web.debugerror
    app = web.application(urls, globals(), web.reloader)
    app.run()

