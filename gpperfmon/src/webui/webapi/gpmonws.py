#!/usr/bin/env python

import os, sys, socket, ConfigParser
import time, datetime, tempfile
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

urls = ('/config', 'config', 
        '/hosts', 'hosts',
        '/database', 'database',
        '/system', 'system',
        '/query', 'query',
        '/queryplan', 'queryplan',
        '/iterator', 'iterator',
        '/logon', 'logon',
        '/logoff', 'logoff',
        '/crossdomain.xml', 'crossdomain'
)


app = web.application(urls, locals())
session = web.session.Session(app, 
                              web.session.DiskStore(tempfile.mkdtemp()), 
                              initializer={'user': '', 'role': {}, 
                                           'loggedin': 0})
render = web.template.render('./templates/', cache=False)

def authrole(username, password):
    (valid, issuper) = db.get_role(username, password)
    # DEBUGGING 
    sys.stderr.write("\nAUTH:\n\tvalid:  %s\n\tsuperuser: %s\n" % (valid, issuper))
    # DEBUGGING 
    if valid:
        session.user = username
        session.role['superuser'] = issuper
        session.role['user'] = True
        session.loggedin = True
    else:
        sys.stderr.write("Authentication failed: %s\n\n" % username)

def chkrole(role):
    return not (role and (not session.user or not session.role[role]))

def mkerr(code, msg):
    return render.error(web.ctx.fullpath, code, msg)

def timenow():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')

class crossdomain:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        return render.crossdomain()




class logon:
    def GET(self):
        i = web.input()
        username = ''
        password = ''
        if i.has_key('username'):
            username = i['username']
        if i.has_key('password'):
            password = i['password']

        # authenticate user
        authrole(username, password)
        if not session.loggedin:
            return web.webapi.accessdenied('<br><center><h1>logon failed</h1></center>')
        
        role = ''
        if session.role['superuser']:
            role = 'superuser, user'
        else:
            role = 'user'

        # DEBUGGING 
        sys.stderr.write("\nLOGON:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 
        return render.logon(username, 'SUCCESS', role, session.getid())

class logoff:
    def GET(self):
        logoffdata = render.logoff(session.user, 'SUCCESS')
        session.kill()
        # DEBUGGING 
        sys.stderr.write("\nLOGOFF:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 

        return logoffdata

class config:
    stime = None
    filename = "/usr/local/lighttpd/www/pages/gpperfcon.conf"
    # configuration section names
    gpmmon = "GPMMON"
    ws = "WS"
    system = "SYSTEM METRICS"
    query = "QUERY METRICS"

    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nCONFIG:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 
        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')
        if config.stime == None:
            # retrieve the first date that monitoring was turned on.
            config.stime = db.start_date()

        # declare configuration settings
        quantum = 0
        s_load2 = 0
        s_cpu = 0
        s_mem = 0
        s_disk = 0
        s_net = 0
        q_cpu_elapsed = 0
        q_cpu_skew = 0

        # parse configuration file
        configuration = ConfigParser.SafeConfigParser()
        configuration.read(config.filename)
        # read GPMMON settings
        if configuration.has_section(config.gpmmon):
            try:
                quantum = configuration.get(config.gpmmon, "quantum")
            except:
                # seting not defined in configuration file
                pass
        # read WS settings
        if configuration.has_section(config.ws):
            try:
                interval.max_data_points = configuration.getint(config.ws, "max_data_points")
            except:
                # seting not defined in configuration file
                pass
        # read SYSTEM METRICS settings
        if configuration.has_section(config.system):
            try:
                s_load2 = configuration.getint(config.system, "load2_threshold")
            except:
                # seting not defined in configuration file
                pass
            try:
                s_cpu = configuration.getint(config.system, "cpu_threshold")
            except:
                # seting not defined in configuration file
                pass
            try:
                s_mem = configuration.getint(config.system, "memory_threshold")
            except:
                # seting not defined in configuration file
                pass
            try:
                s_disk = configuration.getint(config.system, "disk_threshold")
            except:
                # seting not defined in configuration file
                pass
            try:
                s_net = configuration.getint(config.system, "network_threshold")
            except:
                # seting not defined in configuration file
                pass
        # read QUERY METRICS settings
        if configuration.has_section(config.query):
            try:
                q_cpu_elapsed = configuration.getint(config.query, "cpu_elapsed_threshold")
            except:
                # seting not defined in configuration file
                pass
            try:
                q_cpu_skew = configuration.getint(config.query, "skew_cpu_threshold")
            except:
                # seting not defined in configuration file
                pass

        return render.config(config.stime, quantum, interval.max_data_points, \
                        s_load2, s_cpu, s_mem, s_disk, s_net, q_cpu_elapsed, q_cpu_skew)



class hosts:
    nodes = None

    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nHOSTS:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 

        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')
        if hosts.nodes == None:
            # retrieve the list of nodes in the GP DB system
            hosts.nodes = db.hosts()

        # return start time
        return render.hosts(hosts.nodes)


class system:
    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nSYSTEM:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 
        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')

        i = web.input(start='last', end='', host='', interval='minute')

        # DEBUGGING 
        sys.stderr.write("\n\n\n<system metrics>\n")
        sys.stderr.write("\tstart = %s\n\tend = %s\n\tinterval = %s\n\thost = %s\n" % (i.start, i.end, i.interval, i.host))
        sys.stderr.write("</system metrics>\n\n\n")
        # DEBUGGING 

        i.start = i.start.lower()
        i.end = i.end.lower()

        res = None

        if i.start == 'last':
            res = db.system_now(i.host)

        elif i.start != '':
            if i.end == 'last':
                i.interval = interval.check(i.start, timenow(), i.interval)
                res = db.system_upto_now(i.host, i.start, i.interval)

            elif i.end != '':
                i.interval = interval.check(i.start, i.end, i.interval)
                res = db.system_history(i.host, i.start, i.end, i.interval)

        else:
            return mkerr(error.BADREQ, 'missing parameter: "end date"')

        (tkeys, hkeys, ckeys, tab) = res

        # check if the result set is empty
        stime = ''
        etime = ''
        if ckeys:
            if tkeys:
                stime = tkeys[0]; etime = tkeys[-1]
            else:
                stime = timenow(); etime = stime

        return render.system(stime, etime, tkeys, hkeys, ckeys, tab)

class query:
    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nQUERY:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 
        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')

        i = web.input(start='last', end='', interval='minute')

        # DEBUGGING 
        sys.stderr.write("\n\n\n<queries metrics>\n")
        sys.stderr.write("\tstart = %s\n\tend = %s\n\tinterval = %s\n" % (i.start, i.end, i.interval))
        sys.stderr.write("</queries metrics>\n\n\n")
        # DEBUGGING 

        i.start = i.start.lower()
        i.end = i.end.lower()

        # check permissions
        username = ''
        if not session.role['superuser']:
            username = sessions.user

        res = None

        if i.start == 'last':
            res = db.queries_now(username)

        elif i.end == 'last':
            i.interval = interval.check(i.start, timenow(), i.interval)
            res = db.queries_upto_now(i.start, username)

        elif i.end != '':
            i.interval = interval.check(i.start, i.end, i.interval)
            res = db.queries_history(i.start, i.end, username)

        else:
            return mkerr(error.BADREQ, 'missing parameter: "end date"')

        (tkeys, ckeys, tab) = res

        # check if the result set is empty
        stime = ''
        etime = ''
        if ckeys:
            if tkeys:
                stime = tkeys[0]; etime = tkeys[-1]
            else:
                stime = timenow(); etime = stime

        # DEBUGGING 
        sys.stderr.write('RESULTS:\n=======\n\n')
        for t in tkeys:
            sys.stderr.write('\ntime: %s\n' % t)
            sys.stderr.write('\t%s\n' % tab[t])
        sys.stderr.write('start time: %s\n' % stime)
        sys.stderr.write('end time: %s\n' % etime)
        # DEBUGGING 

        return render.query(stime, etime, tkeys, ckeys, tab)


class queryplan:
    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nQUERYPLAN:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 
        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')

        i = web.input(start='', tmid='', ssid='', ccnt='', segid='0')

        # DEBUGGING 
        sys.stderr.write("\n\n\n<query plan>\n")
        sys.stderr.write("\tsegid = %s\n\ttmid = %s\n\tssid = %s\n\tccnt = %s\n" % (i.segid, i.tmid, i.ssid, i.ccnt))
        sys.stderr.write("</query plan>\n\n\n")
        # DEBUGGING 

        if i.tmid == '':
            return mkerr(error.BADREQ, 'missing parameter: "tmid"')
        if i.ssid == '':
            return mkerr(error.BADREQ, 'missing parameter: "ssid"')
        if i.ccnt == '':
            return mkerr(error.BADREQ, 'missing parameter: "ccnt"')

        # verify the query ID is composed of numbers only
        try:
            int(i.tmid)
            int(i.ssid)
            int(i.ccnt)
        except:
            return mkerr(error.BADREQ, 'invalid query ID')

        if i.start == 'last':
            res = db.queryplan_now(i.tmid, i.ssid, i.ccnt, i.segid)

        else:
            res = db.queryplan_history(i.tmid, i.ssid, i.ccnt, i.segid)

        (tab, tree, root) = res

        # DEBUGGING 
        sys.stderr.write('RESULTS:\n=======\n\n')
        sys.stderr.write('tree: %s\n' % tree)
        sys.stderr.write('\ntable:\n')
        for row in tab:
            sys.stderr.write('\t%s => %s\n\n' % (tab[row]['pnid'], tab[row]['nid']))
        # DEBUGGING 

        return render.queryplan(i.tmid, i.ssid, i.ccnt, tab, tree, root)


class database:
    def GET(self):
        # DEBUGGING 
        sys.stderr.write("\nDATABASE:\n")
        sys.stderr.write("session: %s\n" % session.getid())
        sys.stderr.write("user: %s\n" % session.user)
        sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        # DEBUGGING 

        if not session.loggedin:
            return web.webapi.accessdenied()

        web.header('Content-Type', 'text/xml')

        i = web.input(start='last', end='', interval='minute')

        # DEBUGGING 
        sys.stderr.write("\n\n<database>\n")
        sys.stderr.write("\tstart = %s\n\tend = %s\n\tinterval = %s\n" % (i.start, i.end, i.interval))
        sys.stderr.write("</database>\n\n")
        # DEBUGGING 

        i.start = i.start.lower()
        i.end = i.end.lower()

        res = None

        if i.start == 'last':
            res = db.database_now()

        elif i.start != '':
            if i.end == 'last':
                i.interval = interval.check(i.start, timenow(), i.interval)
                res = db.database_upto_now(i.host, i.start, i.interval)

            elif i.end != '':
                i.interval = interval.check(i.start, i.end, i.interval)
                res = db.database_history(i.host, i.start, i.end, i.interval)

        else:
            return mkerr(error.BADREQ, 'missing parameter: "end date"')

        (tkeys, ckeys, tab) = res

        # check if the result set is empty
        stime = ''
        etime = ''
        if ckeys:
            if tkeys:
                stime = tkeys[0]; etime = tkeys[-1]
            else:
                stime = timenow(); etime = stime

        # DEBUGGING 
        sys.stderr.write('RESULTS:\n=======\n\n')
        for t in tkeys:
            sys.stderr.write('\ntime: %s\n' % t)
            sys.stderr.write('\t%s\n' % tab[t])
        sys.stderr.write('start time: %s\n' % stime)
        sys.stderr.write('end time: %s\n' % etime)
        # DEBUGGING 

        return render.database(stime, etime, tkeys, ckeys, tab)



if __name__ == '__main__':
    app.run()

