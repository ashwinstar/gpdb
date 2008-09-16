import os, sys, time, datetime, tempfile
import error
import interval

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
        '/system', 'system',
        '/logon', 'logon',
        '/logoff', 'logoff'
)


app = web.application(urls, locals())
session = web.session.Session(app, 
                              web.session.DiskStore(tempfile.mkdtemp()), 
                              initializer={'user': '', 'role': {}, 
                                           'loggedin': 0})
render = web.template.render('./templates/', cache=False)

def authrole(username, password):
    (valid, issuper) = db.get_role(username, password)
    if valid:
        session.user = username
        session.role['superuser'] = issuper
        session.role['user'] = True
        session.loggedin = True

def chkrole(role):
    return not (role and (not session.user or not session.role[role]))

def mkerr(code, msg):
    return render.error(web.ctx.fullpath, code, msg)

def timenow():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')

class startdate:
    stime = None

    def GET(self):
#        if not session.loggedin: 
#            web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')
        if startdate.stime == None:
            # retrieve the first date that monitoring was turned on.
            startdate.stime = db.start_date()

        # return start time
        return render.startdate(startdate.stime)

class logon:
    def GET(self):
        if session.loggedin:
            session.kill()

        i = web.input()
        username = ''
        password = ''
        if i.has_key('username'):
            username = i['username']
        if i.has_key('password'):
            password = i['password']

        authrole(username, password)
        if not session.loggedin:
            authdata = render.logon(username, 'FAIL', 'None')
            return web.webapi.accessdenied(authdata, 'text/xml')
        
        role = ''
        if session.role['superuser']:
            role = 'superuser, user'
        else:
            role = 'user'

        return render.logon(username, 'SUCCESS', role)

class logoff:
    def GET(self):
        logoffdata = render.logoff(session.user, 'SUCCESS')
        session.kill()
        return logoffdata

class hosts:
    nodes = None

    def GET(self, ses=None):
#        if not session.loggedin:
#            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')
        if hosts.nodes == None:
            # retrieve the list of nodes in the GP DB system
            hosts.nodes = db.hosts()

        # return start time
        return render.hosts(hosts.nodes)


class system:
    def GET(self):
#        if not session.loggedin:
#            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')

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
            i.interval = interval.check(i.start, timenow(), i.interval)
            res = db.metrics_upto_now(i.host, i.start, i.interval)
            if not res:
                return mkerr(error.DB, 'db.metrics_upto_now failed')

        else:
            i.interval = interval.check(i.start, i.end, i.interval)
            res = db.metrics_history(i.host, i.start, i.end, i.interval)
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

