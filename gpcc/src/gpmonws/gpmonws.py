import os, sys, datetime, tempfile, error

gphome = os.environ.get('GPHOME')
if not gphome:
    sys.exit('GPHOME env not set')
datadir = os.getenv('MASTER_DATA_DIRECTORY', None)
if not datadir:
    sys.exit('MASTER_DATA_DIRECTORY env not set')
if not os.path.isdir(datadir):
    sys.exit('MASTER_DATA_DIRECTORY "%s" is not valid' % datadir)

sys.path.append(os.path.join(sys.path[0], 'lib'))
sys.path.append(os.path.join(gphome, 'bin', 'lib'))
import pg8000
import web
import db

web.webapi.internalerror = web.debugerror
web.config.session_parameters.timeout = 1800

urls = ('/startdate', 'startdate', 
        '/hosts', 'hosts',
        '/system', 'system')


app = web.application(urls, locals(), web.reloader)
session = web.session.Session(app, 
                              web.session.DiskStore(tempfile.mkdtemp()), 
                              initializer={'user': '', 'role': {}})
render = web.template.render('./templates/', cache=False)



def chkrole(role):
    return not (role and (not session.user or not session.role[role]))

def mkerr(code, msg):
    return render.error(web.ctx.fullpath, code, msg)

def timenow():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')

class startdate:
    stime = None

    def GET(self, ses=None):
        web.header('Content-Type', 'text/xml')
        if startdate.stime == None:
            # retrieve the first date that monitoring was turned on.
            startdate.stime = db.start_date()

        # return start time
        return render.startdate(startdate.stime)


class hosts:
    nodes = None

    def GET(self, ses=None):
        web.header('Content-Type', 'text/xml')
        if hosts.nodes == None:
            # retrieve the list of nodes in the GP DB system
            hosts.nodes = db.hosts()

        # return start time
        return render.hosts(hosts.nodes)


class system:
    def GET(self):
        web.header('Content-Type', 'text/xml')

        # if not chkrole('user'): return error('not authorized')

        i = web.input(start='last', end='', prev='', host='', interval='', sd='')
        i.start = i.start.lower()
        i.end = i.end.lower()

        res = None
        if i.start == 'next':
            if not i.prev:
                return mkerr(error.BADREQ, 'missing "prev" param')
            res = db.metrics_next(i.prev, i.host)
            if not res:
                return mkerr(error.DB, 'db.metrics_next failed')

        elif i.start == 'last':
            res = db.metrics_now(i.host)
            if not res:
                return mkerr(error.DB, 'db.metrics_now failed')

        elif i.end == 'last':
            res = db.metrics_upto_now(i.host, i.start)
            if not res:
                return mkerr(error.DB, 'db.metrics_upto_now failed')

        else:
            res = db.metrics_history(i.host, i.start, i.end)
            if not res:
                return mkerr(error.DB, 'db.metrics_history failed')

        (tkeys, hkeys, ckeys, tab) = res
        if tkeys:
            stime = tkeys[0]; etime = tkeys[-1]
        else:
            stime = timenow(); etime = stime

        return render.system(stime, etime, tkeys, hkeys, ckeys, tab)

if __name__ == '__main__':
    app.run()

