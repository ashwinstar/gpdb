#!/usr/bin/env python

import os, sys, socket, re, ConfigParser
import time, datetime, tempfile
from socket import gethostname

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
from greenplum import db
from greenplum import error
from greenplum import interval

DEBUG = True

web.config.session_parameters.timeout = 1800
MAX_AGGREGATED_SAMPLES = 2000
MAX_NONAGGREGATED_SAMPLES = 16
MAX_QUERIES_PER_SEARCH = 250
remote = False
allow_trust_logon = False
displayed_server_name = ''

urls = ('/', 'index',
        '/api', 'api',
        '/logon', 'logon',
        '/logoff', 'logoff',
        '/config', 'config', 
        '/hosts', 'hosts',
        '/database', 'database',
        '/system', 'system',
        '/queries', 'queries',
        '/querydetails', 'querydetails',
        '/queryplan', 'queryplan' #,
#        '/iterator', 'iterator'  # Needs overhaul -- not functional
)


app = web.application(urls, locals())
session = web.session.Session(app, 
                              web.session.DiskStore(tempfile.mkdtemp()), 
                              initializer={'user': '', 'role': {}, 
                                           'loggedin': 0})

render = web.template.render('./templates/', cache=False)


# Flex doesn't expose the body of non-200 errors so returning 200 with
# structured error in contents
def page_not_found():
    web.ctx.headers = [('Content-Type', 'text/xml')]
    web.ctx.status = '200 OK';
    return mkerr(error.BADREQ, 'Invalid command')

def internal_error():
    web.ctx.headers = [('Content-Type', 'text/xml')]
    web.ctx.status = '200 OK';
    (type, value) = sys.exc_info()[:2]
    return mkerr(error.INTERNAL, '%s: %s' % (type, value))

app.notfound = page_not_found
#app.internalerror = web.debugerror
app.internalerror = internal_error


def authrole(username, password):
    (valid, issuper, message) = db.get_role(username, password, allow_trust_logon)
    if DEBUG:
        sys.stderr.write("\nAUTH:\n\tvalid:  %s\n\tsuperuser: %s\n" % (valid, issuper))
    if valid:
        session.user = username
        session.role['superuser'] = issuper
        session.role['user'] = True
        session.loggedin = True
    else:
        if DEBUG:
            sys.stderr.write("Authentication failed: %s\n\n" % username)
    return message

def chkrole(role):
    return not (role and (not session.user or not session.role[role]))

def mkerr(code, msg):
    return render.error(web.ctx.fullpath, code, msg)

def timenow():
    return datetime.datetime.now().replace(microsecond=0).isoformat(' ')

class index:
    def GET(self):
        web.header('Content-Type', 'text/html')
        web.header('Cache-Control', 'no-cache')
        host_and_port = web.ctx.host.split(':',1)
        if DEBUG:
            sys.stderr.write("host=%s, port=%s, server_name=%s\n" % (host_and_port[0], host_and_port[1], displayed_server_name))
        return render.index(web.ctx.protocol, host_and_port[0], host_and_port[1], displayed_server_name)

class api:
    def __init__(self):
        self.version = '2.0.0.0'

    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        tab = []
        for i in range(len(urls)/2-1):
            j = (i+1) * 2 + 1
            tab.append(urls[j])
        return render.api(tab, self.version)


class logon:
    def __init__(self):
        self.sanitize = re.compile('^\w+$')

    # We don't want username/password details saved in the web logs,
    # so do the logon via POST. We've added a simple form for GET that
    # allows testing the logon via a web browser.
    def GET(self):
        web.header('Content-Type', 'text/html')
        return render.logon_form();

    def POST(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        i = web.input()
        session.user = ''
        session.role = {}
        session.loggedin = 0
        username = ''
        password = ''
        if i.has_key('username'):
            username = i['username']
        else:
            return mkerr(error.ACCESS_DENIED, 'Username required')
        if i.has_key('password'):
            password = i['password']
        else:
            return mkerr(error.ACCESS_DENIED, 'Password required')

        try:
            s = self.sanitize.search(username)
            if s != None:
                # valid string
                username = s.group()
            else:
                # string contains invalid characters
                return mkerr(error.ACCESS_DENIED, 'Invalid username')
        except:
            return mkerr(error.ACCESS_DENIED, 'Invalid username')

        # authenticate user
        message = ''
        try:
            message = authrole(username, password)
        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, message)
        
        role = ''
        if session.role['superuser']:
            role = 'superuser'
        else:
            role = 'user'

        if DEBUG:
            sys.stderr.write("\nLOGON:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        return render.logon(username, 'SUCCESS', role, session.getid())

class logoff:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        logoffdata = render.logoff(session.user, 'SUCCESS')
        session.kill()
        if DEBUG:
            sys.stderr.write("\nLOGOFF:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))

        return logoffdata

class config:
    def __init__(self):
        global remote, displayed_server_name, allow_trust_logon
        
        # Holds the complete configuration
        self.gpperfmon_config = {}

        filename = datadir + '/gpperfmon/conf/gpperfmon.conf'
        try:
            # parse configuration file
            cfg = ConfigParser.SafeConfigParser()
            cfg.readfp(open(filename))
        except:
            sys.exit("FATAL: Unable to load configuration file: %s\n" % filename)

        for section in cfg.sections():
            section_elements = {}
            for item in cfg.items(section):
                section_elements[item[0].replace(" ","_")] = item[1]
            self.gpperfmon_config[section.replace(" ","_")] = section_elements

        if DEBUG:
            sys.stderr.write("Configuration file: %s\n\n" % self.gpperfmon_config)

        # look for specific values
        try:
            remote = cfg.getboolean("WEB APP", "remote")
        except:
            remote = False
        if DEBUG:
            sys.stderr.write("WEB APP/remote = %s\n" % remote)

        # look for specific values
        try:
            displayed_server_name = cfg.get("WEB APP", "server_name")
        except:
            displayed_server_name = gethostname()
        if DEBUG:
            sys.stderr.write("WEB APP/server_name = %s\n" % displayed_server_name)

        # look for specific values
        try:
            allow_trust_logon = cfg.get("WEB APP", "allow_trust_logon")
        except:
            allow_trust_logon = False
        if DEBUG:
            sys.stderr.write("WEB APP/allow_trust_logon = %s\n" % allow_trust_logon)


    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nCONFIG:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        return render.config(self.gpperfmon_config)
    

class hosts:
    nodes = None

    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nHOSTS:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))

        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        if hosts.nodes == None:
            # retrieve the list of nodes in the GP DB system
            try:
                hosts.nodes = db.hosts()
            except db.GPDBError, errorinfo:
                return mkerr(error.DATA_ACCESS, errorinfo)

        # return start time
        return render.hosts(hosts.nodes)


class system:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nSYSTEM:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(start='NOW', end='NOW', interval='1min', aggregated='yes')

        if DEBUG:
            sys.stderr.write("\n\n\n<system metrics>\n")
            sys.stderr.write("\tstart = %s\n\tend = %s\n\tinterval = %s\n\taggregated = %s\n" % (i.start, i.end, i.interval, i.aggregated))
            sys.stderr.write("</system metrics>\n\n\n")

        res = None

        try:
            if i.start == 'NOW':
                res = db.system_now(remote)
                i.aggregated = 'no'
            else:
                # Only allow superuser to look up historical information
                if not session.role['superuser']:
                    return mkerr(error.NOT_AUTHORIZED, 'Must be database superuser to view historical information')

                i.aggregated = i.aggregated.lower()
                if not i.aggregated in ['yes', 'no']:
                    return mkerr(error.BADREQ, 'Invalid aggregated parameter')

                if i.end == 'NOW':
                    i.end = timenow()

                # validate the time strings, and check the number of samples
                (status, interval_code, samples) = interval.calc_samples(i.start, i.end, i.interval)
                if not status:
                    return mkerr(error.BADREQ, 'Invalid date parameters')

                if i.aggregated == 'yes' and samples > MAX_AGGREGATED_SAMPLES:
                    return mkerr(error.BADREQ, 'Exceeded maximum number of samples for specified interval')
                elif i.aggregated == 'no' and samples > MAX_NONAGGREGATED_SAMPLES:
                    return mkerr(error.BADREQ, 'Exceeded maximum number of samples for specified interval')

                res = db.system_history(i.start, i.end, interval_code, i.aggregated)

        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        (tkeys, hkeys, ckeys, tab) = res

        # check if the result set is empty
        if ckeys:
            if i.aggregated == 'yes':
                return render.system_aggregated(i.start, i.end, i.interval, tkeys, ckeys, tab)
            else:
                return render.system_nonaggregated(i.start, i.end, i.interval, tkeys, hkeys, ckeys, tab)
        else:
            return render.system_noresult(i.start, i.end, i.interval)

class queries:
    def __init__(self):
        self.sanitize = re.compile('^\w+$')
        self.valid_status = ['done', 'start', 'submit', 'abort']


    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nQUERIES:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(start='NOW', end='NOW', status='', textlength='', mintime='', username='', db='')

        try:
            if i.textlength != '':
                i.textlength = int(i.textlength)

            # this value is in seconds
            if i.mintime != '':
                i.mintime = int(i.mintime)

            if i.username != '':
                s = self.sanitize.search(i.username)
                if s != None:
                    # valid string
                    i.username = s.group()
                else:
                    # string contains invalid characters
                    return mkerr(error.BADREQ, 'Invalid username')
                # If not superuser, return error if specified search for other user
                if (not session.role['superuser']) and (i.username != session.user):
                    return mkerr(error.NOT_AUTHORIZED, 'Must be database superuser to do search of other users')
            elif not session.role['superuser']:
                # If not superuser, only see my queries
                i.username = session.user

            if i.db != '':
                s = self.sanitize.search(i.db)
                if s != None:
                    # valid string
                    i.db = s.group()
                else:
                    # string contains invalid characters
                    return mkerr(error.BADREQ, 'Invalid database name')
        except:
            return mkerr(error.BADREQ, 'Invalid request parameter(s)')

        # sanitize status input
        # status can be a '|' separated list of multiple values
        if i.status != '':
            i.status = re.split('[\|]', i.status)
            
            for status in i.status:
                if not status in self.valid_status:
                    return mkerr(error.BADREQ, 'Invalid status value')

        if DEBUG:
            sys.stderr.write("\n<queries metrics>\n")
            sys.stderr.write("\tusername = %s\n" % (i.username))
            sys.stderr.write("\tstart = %s\n\tend = %s\n\tstatus = %s\n" % (i.start, i.end, i.status))
            sys.stderr.write("\tdatabase = %s\n\ttextlength = %s\n\tmintime = %s\n" % (i.db, i.textlength, i.mintime))
            sys.stderr.write("</queries metrics>\n\n\n")

        res = None

        try:
            if i.start == 'NOW':
                res = db.queries_now(remote, i.status, i.username, i.db, i.textlength)
            else:
                # check_valid() sanitizes input against SQL injection attacks 
                try:
                    interval.check_valid(i.start)
                    if i.end != 'NOW':
                        interval.check_valid(i.end)
                except ValueError:
                    return mkerr(error.BADREQ, 'Invalid date parameters')

                res = db.queries_history(i.status, i.start, i.end, i.username, i.mintime, i.db, i.textlength, MAX_QUERIES_PER_SEARCH)

        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        (tkeys, ckeys, tab, row_count) = res

        # Did we hit the maximum number of rows for a search?
        if row_count >= MAX_QUERIES_PER_SEARCH:
            hit_row_max = 'true'
        else:
            hit_row_max = 'false'

        return render.queries(i.start, i.end, tkeys, ckeys, tab, row_count, hit_row_max)

class querydetails:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nQUERYDETAILS:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(id='', realtime='')

        if i.id == '':
            return mkerr(error.BADREQ, 'Must specify query ID parameter')

        id_split = i.id.split('-')

        if len(id_split) != 3:
            return mkerr(error.BADREQ, 'Invalid query ID')

        # sanitize input against SQL injection attacks 
        try:
            for item in id_split:
                int(item)
        except:
            return mkerr(error.BADREQ, 'Invalid query ID')

        if i.realtime == '':
            return mkerr(error.BADREQ, 'Missing realtime parameter')

        i.realtime = i.realtime.lower()
        if not i.realtime in ['yes', 'no']:
            return mkerr(error.BADREQ, 'Invalid realtime parameter')

        if DEBUG:
            sys.stderr.write("\n\n\n<query details>\n")
            sys.stderr.write("\ttmid = %s\n\tssid = %s\n\tccnt = %s\n" % (id_split[0], id_split[1], id_split[2]))
            sys.stderr.write("</query details>\n\n\n")

        # Ensure that only superusers can find other's queries
        username = ''
        if not session.role['superuser']:
            username = session.user
            
        try:
            if i.realtime == 'yes':
                res = db.querydetails_now(remote, id_split[0], id_split[1], id_split[2], username)
            else:
                res = db.querydetails_history(id_split[0], id_split[1], id_split[2], username)
        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        (tkeys, ckeys, tab, row_count) = res

        return render.querydetails(tkeys, ckeys, tab, row_count)



class queryplan:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nQUERYPLAN:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))
        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(id='', realtime='')

        if i.id == '':
            return mkerr(error.BADREQ, 'Must specify query ID parameter')

        id_split = i.id.split('-')

        if len(id_split) != 3:
            return mkerr(error.BADREQ, 'Invalid query ID')

        # sanitize input against SQL injection attacks
        try:
            for item in id_split:
                int(item)
        except:
            return mkerr(error.BADREQ, 'Invalid query ID')

        if i.realtime == '':
            return mkerr(error.BADREQ, 'Missing realtime parameter')

        i.realtime = i.realtime.lower()
        if not i.realtime in ['yes', 'no']:
            return mkerr(error.BADREQ, 'Invalid realtime parameter')

        if DEBUG:
            sys.stderr.write("\n\n\n<query plan>\n")
            sys.stderr.write("\ttmid = %s\n\tssid = %s\n\tccnt = %s\n" % (id_split[0], id_split[1], id_split[2]))
            sys.stderr.write("</query plan>\n\n\n")

        # Ensure that only superusers can find other's queries
        username = ''
        if not session.role['superuser']:
            username = session.user

        try:
            if i.realtime == 'yes':
                res = db.queryplan_now(id_split[0], id_split[1], id_split[2], username)
            else:
                res = db.queryplan_history(id_split[0], id_split[1], id_split[2], username)
        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        (ckeys, tab, tree, root) = res

        if DEBUG:
            sys.stderr.write('RESULTS:\n=======\n\n')
            sys.stderr.write('tree: %s\n' % tree)
            sys.stderr.write('\ntable:\n')
            for row in tab:
                sys.stderr.write('\t%s => %s\n\n' % (tab[row]['pnid'], tab[row]['nid']))

        return render.queryplan(i.id, ckeys, tab, tree, root)


class iterator:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nITERATOR:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))

        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(tmid='', ssid='', ccnt='', nid='')

        if DEBUG:
            sys.stderr.write("\n\n\n<iterator>\n")
            sys.stderr.write("\ttmid = %s\n\tssid = %s\n\tccnt = %s\n\tnid = %s\n" % (i.tmid, i.ssid, i.ccnt, i.nid))
            sys.stderr.write("</iterator>\n\n\n")

        if i.tmid == '':
            return mkerr(error.BADREQ, 'missing parameter: "tmid"')
        if i.ssid == '':
            return mkerr(error.BADREQ, 'missing parameter: "ssid"')
        if i.ccnt == '':
            return mkerr(error.BADREQ, 'missing parameter: "ccnt"')
        if i.nid == '':
            return mkerr(error.BADREQ, 'missing parameter: "nid"')

        # sanitize input against SQL injection attacks 
        try:
            i.tmid = int(i.tmid)
            i.ssid = int(i.ssid)
            i.ccnt = int(i.ccnt)
            i.nid = int(i.nid)
        except:
            return mkerr(error.BADREQ, 'invalid query ID')

        try:
            (skeys, ckeys, tab) = db.queryiterator(i.tmid, i.ssid, i.ccnt, i.nid)
        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        if DEBUG:
            sys.stderr.write('RESULTS:\n=======\n\n')
            sys.stderr.write('iterator:\n')
            for id in skeys:
                sys.stderr.write('\tsegment id: %s\n' %  id)
                for c in ckeys:
                    sys.stderr.write('\t\t%s: %s\n' % (c, tab[id][c]))

        return render.iterator(i.tmid, i.ssid, i.ccnt, i.nid, skeys, ckeys, tab)



class database:
    def GET(self):
        web.header('Content-Type', 'text/xml')
        web.header('Cache-Control', 'no-cache')
        if DEBUG:
            sys.stderr.write("\nDATABASE:\n")
            sys.stderr.write("session: %s\n" % session.getid())
            sys.stderr.write("user: %s\n" % session.user)
            sys.stderr.write("loggedin: %s\n\n" % str(session.loggedin))

        if not session.loggedin:
            return mkerr(error.ACCESS_DENIED, 'You must be logged on to perform this operation')

        i = web.input(start='NOW', end='NOW', interval='1min')
        
        if DEBUG:
            sys.stderr.write("\n\n<database>\n")
            sys.stderr.write("\tstart = %s\n\tend = %s\n\tinterval = %s\n" % (i.start, i.end, i.interval))
            sys.stderr.write("</database>\n\n")

        res = None

        try:
            if i.start == 'NOW':
                res = db.database_now(remote)
            else:
                # Only allow superuser to look up historical information
                if not session.role['superuser']:
                    return mkerr(error.NOT_AUTHORIZED, 'Must be database superuser to see historical information')

                if i.end == 'NOW':
                    i.end = timenow()

                # validate the time strings, and check the number of samples
                (status, interval_code, samples) = interval.calc_samples(i.start, i.end, i.interval)
                if not status:
                    return mkerr(error.BADREQ, 'Invalid date parameters')

                if samples > MAX_AGGREGATED_SAMPLES:
                    return mkerr(error.BADREQ, 'Exceeded maximum number of samples at specified interval')

                res = db.database_history(i.start, i.end, interval_code)

        except db.GPDBError, errorinfo:
            return mkerr(error.DATA_ACCESS, errorinfo)

        (tkeys, ckeys, tab) = res

        # check if the result set is empty
        if ckeys:
            return render.database(i.start, i.end, i.interval, tkeys, ckeys, tab)
        else:
            return render.database_noresult(i.start, i.end, i.interval)
           




if __name__ == '__main__':
    config()
    app.run()

