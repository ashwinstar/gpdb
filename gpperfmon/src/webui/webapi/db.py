import os, sys, datetime

sys.path.append(os.path.join(sys.path[0], 'lib'))
import pg8000

pguser = 'gpmon'
pgdatabase = os.environ.get('PGDATABASE', 'gpperfmon')
pgpass = os.environ.get('PGPASSWORD', '')
pgport = os.environ.get('PGPORT', '5432')
pgport = int(pgport)
pghost = os.environ.get('PGHOST', 'localhost')
path = os.environ.get('MASTER_DATA_DIRECTORY') + '/gpperfmon/data'

agg_func = 'avg'

try:
    conn = pg8000.Connection(user=pguser, password=pgpass, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)
except pg8000.errors.InterfaceError:
    sys.stderr.write('GPDB connection failure: database unavailable\n\n')
    sys.exit()
except pg8000.errors.InternalError:
    sys.stderr.write('GPDB connection failure: invalid user or password\n\n')
    sys.exit()
except:
    sys.stderr.write('GPDB connection failure: %s\n\n' % sys.exc_info()[1])
    conn = None

def get_role(username, password):
    res = (False, False)
    try:
        auth_conn = pg8000.Connection(user=username, password=password, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)

        auth_conn.execute('select rolsuper from pg_roles where rolname = current_user')
        for r in auth_conn.iterate_dict():
            res = (True, r['rolsuper'])

        auth_conn.close()
    except:
        sys.stderr.write('Exception verifying user credentials with database\n\n')

    return res

def start_date():
    # check  connection to database
    if conn == None:
        return None

    try:
        conn.execute('select min(ctime) from system_history')
        for r in conn.iterate_dict():
            return r['min']
    except:
        sys.stderr.write('Exception querying start date\n\n')

    return '2008-01-01 00:00:00'

def hosts():
    # check  connection to database
    if conn == None:
        return None

    rows = []
    try:
        conn.execute('select distinct(hostname) from system_now')
        for r in conn.iterate_dict():
            rows.append(r['hostname'])
    except:
        sys.stderr.write('Exception querying hostnames\n\n')

    return rows


def _system_consolidate(rows):
    td = {}   #time dimension
    hd = {}   #host dimension
    tab = {}  #table where the key is composed of host and time

    x = None
    for r in rows:
        (h, t) = (r['hostname'], r['ctime'])
        td[t] = 1; hd[h] = 1
        tab[(h,t)] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        ckeys = filter(lambda i: i != 'hostname' and i != 'ctime', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    hkeys = hd.keys(); hkeys.sort()
    return (tkeys, hkeys, ckeys, tab)


def _queries_consolidate(rows):
    # verify the result set from the SQL query is not empty
    if not rows:
        return ([], [], {})

    td = {}   #time dimension
    tab = {}  #table where the key is composed of time

    x = None
    for r in rows:
        t = r['ctime']
        td[t] = 1
        tab[t] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        ckeys = filter(lambda i: i != 'ctime', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, ckeys, tab)


def _queryplan_consolidate(rows):

    # verify the result set from the SQL query is not empty
    if not rows:
        return (None, {}, {}, None)

    tree = {} # tree containing the query plan hierarchy
    tab = {} 
    
    x = None
    for r in rows:
        # retrieve parent and child node IDs
        (p, c) = (r['pnid'], r['nid'])
        # create a hash table indexed by parent and child node ID
        tab[c] = r

        # insert new child node into hash table
        if p in tree:
            tree[p].append(c)
        else:
            tree[p] = [c]

    # determine the root of the tree
    nkeys = tree.keys()
    nkeys.sort()
    root = nkeys[0]

    return (tab, tree, root)


def _iterator_consolidate(rows):
    # verify the result set from the SQL query is not empty
    if not rows:
        return ([], [], {})

    segd = {} #time dimension
    tab = {}  #table where the key is composed of segment id

    x = None
    for r in rows:
        s = r['segid']
        segd[s] = 1
        tab[s] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        # filter out segment id
        ckeys = filter(lambda i: i != 'segid', x.keys())
	ckeys.sort()

    skeys = segd.keys(); skeys.sort()
    return (skeys, ckeys, tab)


def _database_consolidate(rows):
    td = {}   #time dimension
    tab = {}  #table where the key is composed time

    x = None
    for r in rows:
        t = r['ctime']
        td[t] = 1
        tab[t] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        ckeys = filter(lambda i: i != 'ctime', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, ckeys, tab)




def system_sql(table, filter, interval):
    columns = [ 'mem_total', 'mem_used', 'mem_actual_used', 'mem_actual_free',
                'swap_total', 'swap_used', 'swap_page_in', 'swap_page_out',
                'cpu_user', 'cpu_sys', 'cpu_idle', 'load0', 'load1', 'load2',
                'quantum', 'disk_ro_rate', 'disk_wo_rate', 'disk_rb_rate',
                'disk_wb_rate', 'net_rp_rate', 'net_wp_rate', 'net_rb_rate',
                'net_wb_rate']

    query_fragment = "select date_trunc('%s', ctime) as ctime, hostname" % interval
    for column in columns:
        query_fragment += ", %s(%s) as %s" % (agg_func, column, column)

    query_fragment += " from %s %s" % (table, filter)
    query_fragment += " group by date_trunc('%s', ctime), hostname " % interval
    return query_fragment


def system_now(host):
    rows = rt_system(host)
    return _system_consolidate(rows)

def rt_system(host):
    filename = "%s/system_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'ctime', 'hostname', 'mem_total', 'mem_used',
                'mem_actual_used', 'mem_actual_free', 
                'swap_total', 'swap_used', 
                'swap_page_in', 'swap_page_out',
                'cpu_user', 'cpu_sys', 'cpu_idle',
                'load0', 'load1', 'load2', 'quantum',
                'disk_ro_rate', 'disk_wo_rate', 
                'disk_rb_rate', 'disk_wb_rate',
                'net_rp_rate', 'net_wp_rate',
                'net_rb_rate', 'net_wb_rate' ]

    rows = []

    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        return rows

    try:
        for line in f:
            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # positional matching. if cnames is modified, the index
            # might be affected.
            if host != '' and entry[1] != host:
                # weed out the entries that do not match the specific
                # hostname
                continue
            # convert date string into datetime object
            entry[0] = datetime.datetime.strptime(entry[0], "%Y-%m-%d %H:%M:%S")
            # append realtime data to the end of the array
            for i in range(len(cnames)):
                columns[cnames[i]] = entry[i]
            rows.append(columns)
    finally:
        f.close()

    # DEBUGGING 
    print rows
    # DEBUGGING 
    return rows



def system_upto_now(host, stime, interval):
    filter = "where ctime >= '%s'" % stime
    if host:
        filter = filter + " and hostname='%s'" % host

    query1 = system_sql('system_history', filter, interval)
    query2 = system_sql('system_tail', filter, interval)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    # query history information.
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    # query real-time information.
    for r in rt_system(host):
        rows.append(r)

    return _system_consolidate(rows)


def system_history(host, stime, etime, interval):
    filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)
    if host:
        filter = filter + " and hostname='%s'" % host

    query1 = system_sql('system_history', filter, interval)
    query2 = system_sql('system_tail', filter, interval)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    return _system_consolidate(rows)



def queries_now(username):
    rows = rt_queries(username)
    return _queries_consolidate(rows)

def queryText(tmid, ssid, ccnt):
    filename = "%s/q%s-%s-%s.txt" % (path, tmid, ssid, ccnt)

    try:
        q = open(filename, 'r')
    except:
        sys.stderr.write("query file not found: %s\n" % filename)
        return ''

    try:
        qtext = ''
        # skip the first line
        q.readline()
        for line in q:
            qtext += line.strip()
    finally:
        q.close()

    return qtext

def rt_queries(username):
    filename = "%s/queries_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'ctime', 'tmid', 'ssid', 'ccnt',
                'uname', 'db', 'cost', 'tsubmit', 
                'tstart', 'tfin', 'status', 'rows_out', 
                'cpu_elapsed', 'cpu_currpct', 
                'skew_cpu', 'skew_rows', 
                'query_hash', 'query_text', 'query_plan' ]

    rows = []

    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        return rows

    try:
        for line in f:
            # DEBUGGING 
            print line
            # DEBUGGING 

            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # skip rows that do not belong to the owner if supplied
            if username != '' and username != entry[4]:
                continue
            # convert ctime date string into datetime object
            entry[0] = datetime.datetime.strptime(entry[0], "%Y-%m-%d %H:%M:%S")
            # convert tsubmit date string into datetime object
            entry[7] = datetime.datetime.strptime(entry[7], "%Y-%m-%d %H:%M:%S")
            # convert tstart date string into datetime object
            entry[8] = datetime.datetime.strptime(entry[8], "%Y-%m-%d %H:%M:%S")
            # if the query hasn't completed, the ftin will be set to 'null'
            if entry[9] != 'null':
                # convert tfin date string into datetime object
                entry[9] = datetime.datetime.strptime(entry[9], "%Y-%m-%d %H:%M:%S")
            for i in range(len(cnames)):
                if cnames[i] == 'query_text':
                    tmid = entry[1]
                    ssid = entry[2]
                    ccnt = entry[3]
                    entry[i] = queryText(tmid, ssid, ccnt)
                columns[cnames[i]] = entry[i]
            rows.append(columns)
    finally:
        f.close()

    return rows


def queries_upto_now(stime, username):
    #filter = "where ctime >= '%s'" % stime
    filter = ''
    if username != '':
        filter = "%s and uname='%s'" % (filter, username)
    query1 = 'select * from queries_history %s' % filter
    query2 = 'select * from queries_tail %s' % filter
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    # query real-time information.
    for r in rt_queries(username):
        rows.append(r)

    return _queries_consolidate(rows)

def queries_history(stime, etime, username):
    #filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)
    filter = ''
    if username != '':
        filter = "%s and uname='%s'" % (filter, username)
    query1 = 'select * from queries_history %s' % filter
    query2 = 'select * from queries_tail %s' % filter
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    return _queries_consolidate(rows)


def queryplan_now(tmid, ssid, ccnt, segid):
    rows = rt_queryplan(tmid, ssid, ccnt, segid)
    return _queryplan_consolidate(rows)

def qp():
    res = queryplan_history('1223590674', '221', '1', '0')
    (tab, tree, root) = res

    stack = [{root : 0}]
    parent = stack[0].keys()[0]
    child = stack[0].values()[0]
    while stack:
        while child < len(tree[parent]):
            indent = ''
            for i in range(parent): indent += '\t'
            print("%s<iterator label=%s nid=%s>" % (indent, tab[tree[parent][child]]['ntype'], tree[parent][child]))
            if tree[parent][child] in tree.keys():
                parent = tree[parent][child]
                stack.append({parent : child})
                child = 0
                break
            indent = ''
            for i in range(parent): indent += '\t'
            print("%s</iterator nid=%s>" % (indent, tree[parent][child]))
            child = child + 1
        else:
            element = stack.pop()
            parent = element.keys()[0]
            child = element.values()[0] + 1
            if not stack:
                break
            if child < len(tree[parent]):
                continue
            indent = ''
            for i in range(parent): indent += '\t'
            print("%s</iterator parent=%s child=%s>" % (indent, parent, child))



def rt_queryplan(tmid, ssid, ccnt, segid=''):
    filename = "%s/iterators_now.dat" % path

    rows = []
    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        return rows

    try:
        for line in f:
            # DEBUGGING 
            print line
            # DEBUGGING 

            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # positional matching. if cnames is modified, the index
            # might be affected.
            if entry[1] != tmid:
                sys.stderr.write("no tmid match: %s != %s\n\n" % (entry[1], str(tmid)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[2] != ssid:
                sys.stderr.write("no ssid match: %s != %s\n\n" % (entry[2], str(ssid)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[3] != ccnt:
                sys.stderr.write("no ccnt match: %s != %s\n\n" % (entry[3], str(ccnt)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif segid != '' and entry[4] != segid:
                sys.stderr.write("no segid match: %s != %s\n\n" % (entry[4], str(segid)))
                continue
            columns['nid'] = entry[6]
            columns['pnid'] = entry[7]
            columns['ntype'] = entry[9]
            rows.append(columns)
    finally:
        f.close()

    return rows


def queryplan_history(tmid, ssid, ccnt, segid):
    query_fragment = "select pnid, nid, ntype"
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    if segid != '':
        filter += " and segid='%s'" % segid

    query1 = "%s from iterators_history %s" % (query_fragment, filter)
    query2 = "%s from iterators_tail %s" % (query_fragment, filter)
    query = "%s\nunion all\n%s order by pnid, nid" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    sys.stderr.write("QUERY RESULTS:\n%s\n\n\n" % rows)

    # query real-time information.
    for r in rt_queryplan(tmid, ssid, ccnt, segid):
        rows.append(r)

    return _queryplan_consolidate(rows)


def rt_queryiterators(tmid, ssid, ccnt, segid='', nid=''):
    filename = "%s/iterators_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'ctime', 'tmid', 'ssid', 'ccnt', 'segid',
                'pid', 'nid', 'pnid', 'hostname', 'ntype',
                'nstatus', 'tstart', 'tduration', 'pmemsize',
                'pmemmax', 'memsize', 'memresid', 'memshare',
                'pagefaults', 'pagefaults_elapsed', 'cpu_elapsed',
                'cpu_currpct', 'phase', 'rowsout', 'rowsout_est',
                'm0_name', 'm0_unit', 'm0_val', 'm0_est',
                'm1_name', 'm1_unit', 'm1_val', 'm1_est',
                'm2_name', 'm2_unit', 'm2_val', 'm2_est',
                'm3_name', 'm3_unit', 'm3_val', 'm3_est',
                'm4_name', 'm4_unit', 'm4_val', 'm4_est',
                'm5_name', 'm5_unit', 'm5_val', 'm5_est',
                'm6_name', 'm6_unit', 'm6_val', 'm6_est',
                'm7_name', 'm7_unit', 'm7_val', 'm7_est',
                'm8_name', 'm8_unit', 'm8_val', 'm8_est',
                'm9_name', 'm9_unit', 'm9_val', 'm9_est',
                'm10_name', 'm10_unit', 'm10_val', 'm10_est',
                'm11_name', 'm11_unit', 'm11_val', 'm11_est',
                'm12_name', 'm12_unit', 'm12_val', 'm12_est',
                'm13_name', 'm13_unit', 'm13_val', 'm13_est',
                'm14_name', 'm14_unit', 'm14_val', 'm14_est',
                'm15_name', 'm15_unit', 'm15_val', 'm15_est' ]

    rows = []
    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        return rows

    try:
        for line in f:
            # DEBUGGING 
            print line
            # DEBUGGING 

            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # positional matching. if cnames is modified, the index
            # might be affected.
            if entry[1] != tmid:
                sys.stderr.write("no tmid match: %s != %s\n\n" % (entry[1], str(tmid)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[2] != ssid:
                sys.stderr.write("no ssid match: %s != %s\n\n" % (entry[2], str(ssid)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[3] != ccnt:
                sys.stderr.write("no ccnt match: %s != %s\n\n" % (entry[3], str(ccnt)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif segid != '' and entry[4] != segid:
                sys.stderr.write("no segid match: %s != %s\n\n" % (entry[4], str(segid)))
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif nid != '' and entry[6] != nid:
                sys.stderr.write("no segid match: %s != %s\n\n" % (entry[6], str(nid)))
                continue
            # convert ctime date string into datetime object
            entry[0] = datetime.datetime.strptime(entry[0], "%Y-%m-%d %H:%M:%S")
            # convert tstart date string into datetime object
            entry[11] = datetime.datetime.strptime(entry[11], "%Y-%m-%d %H:%M:%S")
            for i in range(len(cnames)):
                # do not include the pid if searching based on node id (nid)
                if nid != '' and cnames[i] == 'pid':
                    continue
                columns[cnames[i]] = entry[i]
            rows.append(columns)
    finally:
        f.close()

    return rows



def iterator_sql():
    columns = [ 'segid', 'ntype', 'nstatus', 'tstart', 'tduration',
                'pmemsize', 'pmemmax', 'memsize', 'memresid', 'memshare',
                'pagefaults', 'pagefaults_elapsed',
                'cpu_elapsed', 'cpu_currpct', 'phase',
                'rowsout', 'rowsout_est',
                'm0_name', 'm0_unit', 'm0_val', 'm0_est',
                'm1_name', 'm1_unit', 'm1_val', 'm1_est',
                'm2_name', 'm2_unit', 'm2_val', 'm2_est',
                'm3_name', 'm3_unit', 'm3_val', 'm3_est',
                'm4_name', 'm4_unit', 'm4_val', 'm4_est',
                'm5_name', 'm5_unit', 'm5_val', 'm5_est',
                'm6_name', 'm6_unit', 'm6_val', 'm6_est',
                'm7_name', 'm7_unit', 'm7_val', 'm7_est',
                'm8_name', 'm8_unit', 'm8_val', 'm8_est',
                'm9_name', 'm9_unit', 'm9_val', 'm9_est',
                'm10_name', 'm10_unit', 'm10_val', 'm10_est',
                'm11_name', 'm11_unit', 'm11_val', 'm11_est',
                'm12_name', 'm12_unit', 'm12_val', 'm12_est',
                'm13_name', 'm13_unit', 'm13_val', 'm13_est',
                'm14_name', 'm14_unit', 'm14_val', 'm14_est',
                'm15_name', 'm15_unit', 'm15_val', 'm15_est' ]

    query_fragment = "select ctime"
    for column in columns:
        query_fragment += ", %s" % column

    return query_fragment


# returns iterator data across all segments.
def queryiterator(tmid, ssid, ccnt, nid):
    query_fragment = iterator_sql()
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s' and nid='%s'" % (tmid, ssid, ccnt, nid)

    query1 = "%s from iterators_history %s" % (query_fragment, filter)
    query2 = "%s from iterators_tail %s" % (query_fragment, filter)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    sys.stderr.write("ITERATOR RESULT:\n%s\n\n\n" % rows)

    # query real-time information.
    segid = ''
    for r in rt_queryiterators(tmid, ssid, ccnt, segid, nid):
        rows.append(r)

    return _iterator_consolidate(rows)



def database_now():
    rows = rt_database()
    return _database_consolidate(rows)

def rt_database():
    filename = "%s/database_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'ctime', 'queries_total', 
                'queries_running', 'queries_queued']

    rows = []

    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        return rows

    try:
        for line in f:
            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # convert date string into datetime object
            entry[0] = datetime.datetime.strptime(entry[0], "%Y-%m-%d %H:%M:%S")
            # append realtime data to the end of the array
            for i in range(len(cnames)):
                columns[cnames[i]] = entry[i]
            rows.append(columns)
    finally:
        f.close()

    # DEBUGGING 
    print rows
    # DEBUGGING 
    return rows


def database_sql(table, filter, interval):
    columns = ['queries_total', 'queries_running', 'queries_queued']

    query_fragment = "select date_trunc('%s', ctime) as ctime" % interval
    for column in columns:
        query_fragment += ", %s(%s) as %s" % (agg_func, column, column)

    query_fragment += " from %s %s" % (table, filter)
    query_fragment += " group by date_trunc('%s', ctime) " % interval
    return query_fragment


def database_upto_now(stime, interval):
    filter = "where ctime >= '%s'" % stime

    query1 = database_sql('database_history', filter, interval)
    query2 = database_sql('database_tail', filter, interval)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    # query history information.
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    # query real-time information.
    for r in rt_database():
        rows.append(r)

    return _database_consolidate(rows)


def database_history(stime, etime, interval):
    filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)

    query1 = database_sql('database_history', filter, interval)
    query2 = database_sql('database_tail', filter, interval)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        sys.stderr.write('Exception querying database: %s\n\n' % query)

    return _database_consolidate(rows)



