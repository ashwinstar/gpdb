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

# Use this to throw exceptions related to unavailability of the DB
class GPDBError(Exception):
    def __init__(self, value):
        self.value = value 
    def __str__(self):
        return repr(self.value)

def create_db_conn():
  global conn
  try:
      conn = pg8000.Connection(user=pguser, password=pgpass, host=pghost, port=pgport, database=pgdatabase)
  except pg8000.errors.InterfaceError:
      sys.stderr.write('GPDB connection failure: database unavailable\n\n')
      conn = None
  except pg8000.errors.InternalError:
      sys.stderr.write('GPDB connection failure: invalid user or password\n\n')
      conn = None
  except:
      sys.stderr.write('GPDB connection failure: %s\n\n' % sys.exc_info()[1])
      conn = None

def raise_db_error(text):
    sys.stderr.write(text+'\n\n')
    raise GPDBError, text

def validate_db_conn(): 
  global conn
  if conn == None:
     sys.stderr.write('Lost database connection - attempting to re-establish\n\n')
     create_db_conn()
  if conn == None:
     raise_db_error('Unable to connect to the database')


# Connect to the database
create_db_conn()

def get_role(username, password):
    validate_db_conn()

    res = (False, False)
    try:
        # First make sure the login credentials are valid
        auth_conn = pg8000.Connection(user=username, password=password, host=pghost, port=pgport, database=pgdatabase)
        sys.stderr.write('Logon: validated that the login credentials are valid\n\n')
        auth_conn.close()
    except:
        sys.stderr.write('Logon: attempt to connect with user credentials failed\n\n')
        return res

    try:
        # Then use our superuser connection to check the user's authorization level
        check_auth = 'select rolsuper from pg_roles where rolname = \'%s\'' % username
        conn.execute(check_auth)
   
        for r in conn.iterate_dict():
            res = (True, r['rolsuper'])
        sys.stderr.write('Logon: checked if the user is superuser or not\n\n')
    except:
        raise_db_error('Exception during query: %s ' % check_auth)

    return res

def hosts():
    validate_db_conn()

    rows = []
    try:
        query = 'select distinct(hostname) from system_now'
        conn.execute(query)
        for r in conn.iterate_dict():
            rows.append(r['hostname'])
    except:
        raise_db_error('Exception during query: %s' % query)

    return rows

def _system_consolidate_aggregate(rows):
    td = {}   #time dimension
    tab = {}  #table where the key is composed of host and time

    x = None
    for r in rows:
        t = r['sample_time']
        td[t] = 1;
        tab[t] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        #ckeys = filter(lambda i: i != 'sample_time', x.keys())
        ckeys = x.keys();
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, None, ckeys, tab)


def _system_consolidate_nonaggregate(rows):
    td = {}   #time dimension
    hd = {}   #host dimension
    tab = {}  #table where the key is composed of host and time

    x = None
    for r in rows:
        (h, t) = (r['hostname'], r['sample_time'])
        td[t] = 1; hd[h] = 1
        tab[(h,t)] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        #ckeys = filter(lambda i: i != 'hostname' and i != 'sample_time', x.keys())
        ckeys = filter(lambda i: i != 'hostname', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    hkeys = hd.keys(); hkeys.sort()
    return (tkeys, hkeys, ckeys, tab)


def _queries_consolidate(rows):
    # verify the result set from the SQL query is not empty
    if not rows:
        return ([], [], {}, 0)

    td = {}   #time dimension
    tab = {}  #table where the key is composed of time

    x = None
    row_count = 0
    for r in rows:
        t = '%s-%s-%s' % (r['tmid'], r['ssid'], r['ccnt'])
        td[t] = 1
        tab[t] = r
        x = r
        row_count += 1

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        ckeys = filter(lambda i: i != 'query_hash' and i != 'length' and i != 'query_plan' and i != 'tmid' and i != 'ssid' and i != 'ccnt', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, ckeys, tab, row_count)


def _queryplan_consolidate(rows):

    # verify the result set from the SQL query is not empty
    if not rows:
        return ([], {}, {}, None)

    tree = {} # tree containing the query plan hierarchy
    tab = {} 
    
    x = None
    for r in rows:
        # retrieve parent and child node IDs
        (p, c) = (r['pnid'], r['nid'])
        # create a hash table indexed by parent and child node ID
        tab[c] = r
        x = r
        # insert new child node into hash table
        if p in tree:
            tree[p].append(c)
        else:
            tree[p] = [c]

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        ckeys = filter(lambda i: i != 'tmid' and i != 'ssid' and i != 'ccnt' and i != 'nid' and i != 'pnid', x.keys())
	ckeys.sort()

    # determine the root of the tree
    nkeys = tree.keys()
    nkeys.sort()
    root = nkeys[0]

    return (ckeys, tab, tree, root)


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
        t = r['sample_time']
        td[t] = 1
        tab[t] = r
        x = r

    # retrieve the column names into ckeys
    ckeys = None
    if x:
        #ckeys = filter(lambda i: i != 'sample_time', x.keys())
        ckeys = x.keys();
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, ckeys, tab)



def system_now():
    rows = rt_system()
    return _system_consolidate_nonaggregate(rows)

def rt_system():
    filename = "%s/system_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'sample_time', 'hostname', 'mem_total', 'mem_used',
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
        raise GPDBError, 'Real-time system_now file unavailable'
        return rows

    try:
        for line in f:
            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # positional matching. if cnames is modified, the index
            # might be affected.

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


def system_sql(table, filter, interval_code, aggregate):
    columns = [ 'mem_total', 'mem_used', 'mem_actual_used', 'mem_actual_free',
                'swap_total', 'swap_used', 'swap_page_in', 'swap_page_out',
                'cpu_user', 'cpu_sys', 'cpu_idle', 'load0', 'load1', 'load2',
                'disk_ro_rate', 'disk_wo_rate', 'disk_rb_rate',
                'disk_wb_rate', 'net_rp_rate', 'net_wp_rate', 'net_rb_rate',
                'net_wb_rate']

    # Time aggregation expressions for each level of aggregration
    #  1min, 5min, 1hr, 6hr, 1day, 1wk, 6mth
    itvl_sql = ["date_trunc('minute', ctime)",   
                "date_trunc('hour', ctime)+(((trunc(date_part('minute', ctime)/5)*5)::text||' minutes')::interval)",
                "date_trunc('hour', ctime)",   
                "date_trunc('day', ctime)+(((trunc(date_part('hour', ctime)/6)*6)::text||' hours')::interval)",
                "date_trunc('day', ctime)",   
                "date_trunc('week', ctime)",   
                "date_trunc('year', ctime)+(((trunc(date_part('month', ctime)/6)*6)::text||' months')::interval)"]

    if aggregate:
        query_fragment = "select %s as sample_time" % itvl_sql[interval_code]
        for column in columns:
            query_fragment += ", avg(%s) as %s" % (column, column)

        query_fragment += " from %s %s" % (table, filter)
        query_fragment += " group by sample_time"

    else:
        query_fragment = "select %s as sample_time, hostname" % itvl_sql[interval_code]
        for column in columns:
            query_fragment += ", avg(%s) as %s" % (column, column)

        query_fragment += " from %s %s" % (table, filter)
        query_fragment += " group by hostname, sample_time"

    return query_fragment


def system_history(stime, etime, interval_code, aggregate):
    validate_db_conn()

    if etime == '':
        filter = "where ctime >= '%s'" % stime
    else:
        filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)

    query = system_sql('system_history', filter, interval_code, aggregate)

    sys.stderr.write("system_history query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        raise_db_error('Exception during query: %s' % query)

    if aggregate:
        return _system_consolidate_aggregate(rows)
    else:
        return _system_consolidate_nonaggregate(rows)



def queries_now(status, username, database, textlength):
    rows = rt_queries(status, username, database, textlength, '', '', '')
    return _queries_consolidate(rows)

def queryText(tmid, ssid, ccnt):
    filename = "%s/q%s-%s-%s.txt" % (path, tmid, ssid, ccnt)

    try:
        q = open(filename, 'r')
    except:
        sys.stderr.write("query file not found: %s\n" % filename)
        raise GPDBError, 'Real-time query_text file unavailable'
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

def rt_queries(status, username, database, textlength, tmid, ssid, ccnt):
    filename = "%s/queries_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'sample_time', 'tmid', 'ssid', 'ccnt',
                'username', 'db', 'cost', 'tsubmit', 
                'tstart', 'tfinish', 'status', 'rows_out', 
                'cpu_elapsed', 'cpu_currpct', 
                'skew_cpu', 'skew_rows', 
                'query_hash', 'query_text', 'query_plan' ]

    rows = []

    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        raise GPDBError, 'Real-time queries_now file unavailable'
        return ''

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
            # skip rows that do not match the database desired
            if database != '' and database != entry[5]:
                continue
            # skip rows that do have the status desired
            if status != '' and (entry[10] not in status):
                continue
            # check for tmid, ssid, ccnt
            if tmid != '' and tmid != entry[1]:
                continue
            if ssid != '' and ssid != entry[2]:
                continue
            if ccnt != '' and ccnt != entry[3]:
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
                    if textlength != '':
                        # truncate the query text
                        if textlength > 3 and len(entry[i]) > textlength:
                            entry[i] = entry[i][:textlength-3] + '...'
                columns[cnames[i]] = entry[i]
            rows.append(columns)
    finally:
        f.close()

    return rows


def queries_sql(textlength):
    columns = [ 'ctime as sample_time', 'tmid', 'ssid', 'ccnt',
                'username', 'db', 'cost', 'tsubmit', 
                'tstart', 'tfinish', 'status', 'rows_out', 
                'cpu_elapsed', 'cpu_currpct', 
                'skew_cpu', 'skew_rows', 
                'query_hash', 'query_plan' ]

    if textlength != '':
        query_fragment = "select length(query_text), substr(query_text, 0, %s) as query_text" % textlength
    else:
        query_fragment = "select query_text"

    for column in columns:
        query_fragment += ", %s" % column

    return query_fragment


def queries_history(status, stime, etime, username, runtime, database, textlength, max_query_limit):
    validate_db_conn()

    if etime == '':
        filter = "where '%s' <= tfinish" % stime
    else:
        filter = "where '%s' <= tfinish and tstart <= '%s'" % (stime, etime)

    if status != '':
        status_list = (', '.join("'" + i + "'" for i in status))
        filter += " and status in (%s)" % status_list
    if username != '':
        filter += " and username='%s'" % username
    if database != '':
        filter += " and db='%s'" % database
    if runtime != '':
        filter += " and (tfinish - tstart) >='%s'" % runtime

    columns = queries_sql(textlength)

    query1 = '%s from queries_history %s' % (columns, filter)
    query2 = '%s from queries_tail %s' % (columns, filter)
    query = "(%s) union all (%s) limit %s" % (query1, query2, max_query_limit)

    sys.stderr.write("queries_history query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        for r in conn.iterate_dict():
            if textlength != '':
                if r['length'] > textlength:
                    if textlength > 3:
                        r['query_text'] = r['query_text'][:textlength-3] + '...'
            rows.append(r)
    except:
        raise_db_error('Exception during query: %s' % query)

    return _queries_consolidate(rows)


def querydetails_now(tmid, ssid, ccnt, username):
    rows = rt_queries('', username, '', '', tmid, ssid, ccnt)
    return _queries_consolidate(rows)


def querydetails_history(tmid, ssid, ccnt, username):
    validate_db_conn()

    columns = queries_sql('')

    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    if username != '':
        filter += " and username='%s'" % username
    query1 = '%s from queries_history %s' % (columns, filter)
    query2 = '%s from queries_tail %s' % (columns, filter)
    query = "(%s) union all (%s) limit 1" % (query1, query2)

    sys.stderr.write("querydetails_history query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        raise_db_error('Exception during query: %s' % query)

    return _queries_consolidate(rows)



# TODO: remove this function as testing is done
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



def queryplan_now(tmid, ssid, ccnt, username):
    validate_db_conn()

    query_fragment = "select *"
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)

    # Reminder -- need to insert a filter on username here
    #             for non-superuser queries
    if username != '':
        sys.stderr.write("WARNING: Currently ignoring username filter\n\n")

    query1 = "%s from iterators_now_rollup %s" % (query_fragment, filter)
    query = "%s order by pnid, nid" % query1

    sys.stderr.write("queryplan_now query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        raise_db_error('Exception during query: %s' % query)

    sys.stderr.write("QUERY RESULTS:\n%s\n\n\n" % rows)

    return _queryplan_consolidate(rows)



def queryplan_history(tmid, ssid, ccnt, username):
    validate_db_conn()

    query_fragment = "select *"
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)

    # Reminder -- need to insert a filter on username here
    #             for non-superuser queries
    if username != '':
        sys.stderr.write("WARNING: Currently ignoring username filter\n\n")

    query1 = "%s from iterators_history_rollup %s" % (query_fragment, filter)
    query2 = "%s from iterators_tail_rollup %s" % (query_fragment, filter)
    query = "(%s) union all (%s) order by pnid, nid" % (query1, query2)

    sys.stderr.write("queryplan_now query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        raise_db_error('Exception during query: %s' % query)

    sys.stderr.write("QUERY RESULTS:\n%s\n\n\n" % rows)

    return _queryplan_consolidate(rows)


def rt_queryiterators(tmid, ssid, ccnt, segid='', nid=''):
    filename = "%s/iterators_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'ctime', 'tmid', 'ssid', 'ccnt', 'segid',
                'pid', 'nid', 'pnid', 'hostname', 'ntype',
                'nstatus', 'tstart', 'tduration', 'pmemsize',
                'pmemmax', 'memsize', 'memresid', 'memshare',
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

    rows = []
    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        raise GPDBError, 'Real-time iterators_now file unavailable'
        return ''

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
    validate_db_conn()

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
        raise_db_error('Exception during query: %s' % query)

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
    cnames = [  'sample_time', 'queries_total', 
                'queries_running', 'queries_queued']

    rows = []

    try:
        f = open(filename, 'r')
    except:
        sys.stderr.write("file not found: %s\n" % filename)
        raise GPDBError, 'Real-time database_now file unavailable'
        return ''

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


def database_sql(table, filter, interval_code):
    columns = ['queries_total', 'queries_running', 'queries_queued']

    # Time aggregation expressions for each level of aggregration
    #  1min, 5min, 1hr, 6hr, 1day, 1wk, 6mth
    itvl_sql = ["date_trunc('minute', ctime)",
                "date_trunc('hour', ctime)+(((trunc(date_part('minute', ctime)/5)*5)::text||' minutes')::interval)",
                "date_trunc('hour', ctime)",
                "date_trunc('day', ctime)+(((trunc(date_part('hour', ctime)/6)*6)::text||' hours')::interval)",
                "date_trunc('day', ctime)",
                "date_trunc('week', ctime)",
                "date_trunc('year', ctime)+(((trunc(date_part('month', ctime)/6)*6)::text||' months')::interval)"]


    query_fragment = "select %s as sample_time" % itvl_sql[interval_code]
    for column in columns:
        query_fragment += ", avg(%s) as %s" % (column, column)
        query_fragment += ", min(%s) as %s_min" % (column, column)
        query_fragment += ", max(%s) as %s_max" % (column, column)

    query_fragment += " from %s %s" % (table, filter)
    query_fragment += " group by sample_time"
    return query_fragment


def database_history(stime, etime, interval_code):
    validate_db_conn()

    if etime == '':
        filter = "where ctime >= '%s'" % stime
    else:
        filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)

    query = database_sql('database_history', filter, interval_code)

    sys.stderr.write("database_history query: %s\n" % query)

    rows = []
    try:
        conn.execute(query)
        rows = [r for r in conn.iterate_dict()]
    except:
        raise_db_error('Exception during query: %s' % query)

    return _database_consolidate(rows)



