import os, sys, datetime, locale, time
import gplog

from pygresql import pg

log = None
pguser = 'gpmon'
pgdatabase = 'gpperfmon'
pgpass = os.environ.get('PGPASSWORD', '')
pgport = os.environ.get('PGPORT', '5432')
pgport = int(pgport)
pghost = os.environ.get('PGHOST', 'localhost')
pghost_logon = os.environ.get('PGHOST_LOGON', pghost)
path=''
try:
    path = os.environ.get('MASTER_DATA_DIRECTORY') + '/gpperfmon/data'
except:
    print 'MASTER_DATA_DIRECTORY env not set'

# Use this to throw exceptions related to unavailability of the DB
class GPDBError(Exception):
    def __init__(self, value):
        self.value = value 
    def __str__(self):
        return repr(self.value)

def set_logger(weblog):
    global log
    log = weblog

def raise_db_error(text):
    raise GPDBError, text

def get_quantum(username, password):
    rows = execute_query('SELECT quantum FROM system_now LIMIT 1', username, password)
    if rows and len(rows) > 0:
        return (rows[0])['quantum']
    else:
        return 15

def get_role(username, password, allow_trust_logon):

    res = (False, False, 'Incorrect username/password')

    # First make sure the login credentials are valid

    if not allow_trust_logon:
        if password == None or password.strip() == '':
            log.msg('Trust logon not permiited ... specifify password')
            res = (False, False, "Unprotected 'trust' logon blocked")
            return res
        
    # Now check with password 
    try:
        log.debug('connect to database user=%s password=%s host=%s port=%s database=%s' % (username,password,pghost,pgport,pgdatabase))

        auth_conn = pg.connect(dbname=pgdatabase, host=pghost_logon, port=pgport, user=username, passwd=password)
        auth_conn.close()

    except:
        log.debug('failed connecting to database using password')
        return res

    cnt=2
    while cnt > 0:
        try:
            # Then use our superuser connection to check the user's authorization level
            check_auth_query = 'select rolsuper from pg_roles where rolname = \'%s\'' % username
            for r in execute_query(check_auth_query, username, password):
                res = (True, r['rolsuper'], '')
            break
        except Exception:
            log.msg('error: checking for superuser with query; %s' % check_auth_query)
            raise_db_error('Exception during query: %s ' % check_auth_query)

    return res

def hosts(username, password):
    rows = []
    try:
        query = 'select distinct(hostname) from system_now'
        for r in execute_query(query, username, password):
            rows.append(r['hostname'])
    except:
        log.msg('error during query: %s' % query)
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
        ckeys = x.keys();
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    return (tkeys, ckeys, tab)


def execute_query(query, username, password):

    try:
        conn = pg.connect(dbname=pgdatabase, host=pghost, port=pgport, user=username, passwd=password)
    except Exception, e:
        log.msg('error connecting to GPDB: %s for query: %s' % (e.__str__().strip(), query))
        log.msg("dbname=%s, host=%s, port=%s, user=%s, passwd=%s" % (pgdatabase, pghost, pgport, username, password))
        raise_db_error('Connection error for query: %s' % query)
    except:
        log.msg('error connecting to GPDB for query: %s' % (query))
        log.msg("dbname=%s, host=%s, port=%s, user=%s, passwd=%s" % (pgdatabase, pghost, pgport, username, password))
        raise_db_error('Connection error for query: %s' % query)

    try:
        q = conn.query(query)
        return q.dictresult()
    except Exception, e:
        log.msg('error during query (%s): %s' % (query, e.__str__().strip()))
        raise_db_error('Exception during query: %s' % query)
    except:
        log.msg('error during query: %s' % query)
        raise_db_error('Exception during query: %s' % query)

    return []

# perform a query that we expect ro return a single row with a single value only
def execute_single_value_query(query, username, password):

    rows = execute_query(query, username, password)
    if len(rows) != 1:
        log.msg('Expected to get one row from query (%s) but got %d rows' % (query, len(rows)))
        raise_db_error('Expected to get one row from query (%s) but got %d rows' % (query, len(rows)))
        
    firstRow = rows[0]

    if len(firstRow.keys() != 1):
        log.msg('Expected to get one column from query (%s) but got %d columns' % (query, len(firstRow.keys())))
        raise_db_error('Expected to get one row from query (%s) but got %d rows' % (query, len(firstRow.keys())))

    key = firstRow.keys()[0]
    value = firstRow[key]
    return value

def system_now(remote, username, password):
    if remote:
        nonaggregate = 'no'
        interval_code = 7 # magic number
        query = system_sql('system_now', '', interval_code, nonaggregate)
        rows = execute_query(query, username, password)
    else:
        rows = system_file()

    return _system_consolidate_nonaggregate(rows)


def system_file():
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

    # Scale some of the fields to MBytes
    scaling = [ 1, 1, 1048576, 1048576, 
                1048576, 1048576,
                1048576, 1048576,
                1, 1,
                1, 1, 1,
                1, 1, 1, 1,
                1, 1, 
                1048576, 1048576,
                1, 1, 
                1048576, 1048576 ]
    rows = []

    try:
        f = open(filename, 'r')
    except:
        log.msg('could not open system_now file: %s' % filename)
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
                if scaling[i] == 1:
                    columns[cnames[i]] = entry[i]
                else:
                    columns[cnames[i]] = float(entry[i])/scaling[i]
            rows.append(columns)
    finally:
        f.close()

    return rows


def system_sql(table, filter, interval_code, aggregate):
    columns = [ 'mem_total', 'mem_used', 'mem_actual_used', 'mem_actual_free',
                'swap_total', 'swap_used', 'swap_page_in', 'swap_page_out',
                'cpu_user', 'cpu_sys', 'cpu_idle', 'load0', 'load1', 'load2',
                'disk_ro_rate', 'disk_wo_rate', 'disk_rb_rate',
                'disk_wb_rate', 'net_rp_rate', 'net_wp_rate', 'net_rb_rate',
                'net_wb_rate']

    # Scale some of the fields to MBytes
    scaling = [ 1048576, 1048576, 1048576, 1048576,
                1048576, 1048576, 1, 1,
                1, 1, 1, 1, 1, 1, 
                1, 1, 1048576, 
                1048576, 1, 1, 1048576, 
                1048576 ]

    # Time aggregation expressions for each level of aggregration
    #  1min, 5min, 1hr, 6hr, 1day, 1wk, 6mth
    itvl_sql = ["date_trunc('minute', ctime)",   
                "date_trunc('hour', ctime)+(((trunc(date_part('minute', ctime)/5)*5)::text||' minutes')::interval)",
                "date_trunc('hour', ctime)",   
                "date_trunc('day', ctime)+(((trunc(date_part('hour', ctime)/6)*6)::text||' hours')::interval)",
                "date_trunc('day', ctime)",   
                "date_trunc('week', ctime)",   
                "date_trunc('year', ctime)+(((trunc(date_part('month', ctime)/6)*6)::text||' months')::interval)",
                "ctime"]

    if aggregate == 'yes':
        query_fragment = "select %s as sample_time" % itvl_sql[interval_code]
        for i in range(len(columns)):
            if scaling[i] == 1:
                query_fragment += ", avg(%s) as %s" % (columns[i], columns[i])
            else:
                query_fragment += ", (avg(%s)::real/%s) as %s" % (columns[i], scaling[i], columns[i])

        query_fragment += " from %s %s" % (table, filter)
        query_fragment += " group by sample_time"

    else:
        query_fragment = "select %s as sample_time, hostname" % itvl_sql[interval_code]
        for i in range(len(columns)):
            if scaling[i] == 1:
                query_fragment += ", avg(%s) as %s" % (columns[i], columns[i])
            else:
                query_fragment += ", (avg(%s)::real/%s) as %s" % (columns[i], scaling[i], columns[i])

        query_fragment += " from %s %s" % (table, filter)
        query_fragment += " group by hostname, sample_time"

    return query_fragment


def system_history(stime, etime, interval_code, aggregate, username, password):
    if etime == 'NOW':
        filter = "where ctime >= '%s'" % stime
    else:
        filter = "where '%s' <= ctime and ctime < '%s'" % (stime, etime)

    query1 = system_sql('system_history', filter, interval_code, aggregate)
    query2 = system_sql('system_tail', filter, interval_code, aggregate)
    query = '%s union all %s' % (query1, query2)

    rows = execute_query(query, username, password)

    if aggregate == 'yes':
        return _system_consolidate_aggregate(rows)
    else:
        return _system_consolidate_nonaggregate(rows)


def queries_now(remote, status, username, database, textlength, dbuser, dbpass):
    if remote:
        filter = ''
        if status != '':
            status_list = (', '.join("'" + i + "'" for i in status))
            filter += " where status in (%s)" % status_list

        if username != '':
            if filter == '':
                filter = " where"
            else:
                filter += " and"
            filter += " username='%s'" % username

        if database != '':
            if filter == '':
                filter = " where"
            else:
                filter += " and"
            filter += " db='%s'" % database

        columns = queries_sql(textlength)

        query = '%s from queries_now %s' % (columns, filter)
        rows = queries_db(query, textlength, dbuser, dbpass)
    else:
        rows = queries_file(status, username, database, textlength, '', '', '')

    return _queries_consolidate(rows)


def queryText(tmid, ssid, ccnt):
    filename = "%s/q%s-%s-%s.txt" % (path, tmid, ssid, ccnt)

    try:
        q = open(filename, 'r')
    except:

        log.msg('could not open query text file: %s' % filename)
        # Changing this from exception to just return 'not available'
        #raise GPDBError, 'Real-time query_text file unavailable'
        return '[not available]'

    try:
        qtext = ''
        # skip the first line
        q.readline()
        for line in q:
            qtext += line.rstrip() + '\n'
    finally:
        q.close()

    return qtext.rstrip()[0:-1]


def queries_file(status, username, database, textlength, tmid, ssid, ccnt):
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
        log.msg('could not open queries_now file: %s' % filename)
        raise GPDBError, 'Real-time queries_now file unavailable'
        return ''

    try:
        for line in f:

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
            if entry[8] != 'null':
           	    entry[8] = datetime.datetime.strptime(entry[8], "%Y-%m-%d %H:%M:%S")
            # if the query hasn't completed, the ftin will be set to 'null'
            if entry[9] != 'null':
                # convert tfin date string into datetime object
                entry[9] = datetime.datetime.strptime(entry[9], "%Y-%m-%d %H:%M:%S")
            for i in range(len(cnames)):
                if cnames[i] == 'query_text':
                    _tmid = entry[1]
                    _ssid = entry[2]
                    _ccnt = entry[3]
                    entry[i] = queryText(_tmid, _ssid, _ccnt)
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


def queries_history(status, stime, etime, username, runtime, database, textlength, max_query_limit, dbuser, dbpass):
    if etime == 'NOW':
        filter = "where '%s' <= tfinish" % stime
    else:
        filter = "where '%s' <= tfinish and tstart <= '%s'" % (stime, etime)

    if runtime != '':
        filter += " and (tfinish - tstart) >='%s'" % runtime

    if status != '':
        status_list = (', '.join("'" + i + "'" for i in status))
        filter += " and status in (%s)" % status_list

    if username != '':
        filter += " and username='%s'" % username

    if database != '':
        filter += " and db='%s'" % database

    columns = queries_sql(textlength)

    query1 = '%s from queries_history %s' % (columns, filter)
    query2 = '%s from queries_tail %s' % (columns, filter)
    query = "(%s) union all (%s) limit %s" % (query1, query2, max_query_limit)

    rows = queries_db(query, textlength, dbuser, dbpass)
    return _queries_consolidate(rows)


def queries_db(query, textlength, username, password):
    rows = []
    try:
        for r in execute_query(query, username, password):
            if textlength != '':
                if r['length'] > textlength:
                    if textlength > 3:
                        r['query_text'] = r['query_text'][:textlength-3] + '...'
            rows.append(r)
    except:
        log.msg('error during query: %s' % query)
        raise_db_error('Exception during query: %s' % query)

    return rows


def querydetails_now(remote, tmid, ssid, ccnt, username, dbuser, dbpass):
    if remote:
        columns = queries_sql('')

        filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
        if username != '':
            filter += " and username='%s'" % username
        query = '%s from queries_now %s' % (columns, filter)

        rows = execute_query(query, dbuser, dbpass)
    else:
        rows = queries_file('', username, '', '', tmid, ssid, ccnt)

    return _queries_consolidate(rows)


def querydetails_history(tmid, ssid, ccnt, username, start, end, dbuser, dbpass):
    columns = queries_sql('')

    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    if username != '':
        filter += " and username='%s'" % username
    if start != '' and end != '':
        filter += " and ctime>='%s' and ctime<='%s'" % (start, end)

    query1 = '%s from queries_history %s' % (columns, filter)
    query2 = '%s from queries_tail %s' % (columns, filter)
    query = "(%s) union all (%s) limit 1" % (query1, query2)

    rows = execute_query(query, dbuser, dbpass)

    return _queries_consolidate(rows)



def queryplan_get_username_now(tmid, ssid, ccnt, username, password):
    query_fragment = "select username"
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    query = "%s from queries_now %s limit 1" % (query_fragment, filter)
    
    result = ''
    try:
        r = execute_single_value_query(query, username, password)
        result = '%s' % r
    except:
        log.msg('error during query: %s' % query)
        raise_db_error('Exception during query: %s' % query)
    return result

def queryplan_sql(table, tmid, ssid, ccnt, start='', end=''):
    query = """select
       min(ctime) as sample_time, tmid, ssid, ccnt, nid, min(pnid) as pnid,
       min(ntype) as ntype, array_to_string(iterators_array_accum(distinct nstatus), ',') as nstatus,
       min(tstart) as tstart, avg(tduration) as tduration,
       avg(pmemsize) as pmemsize, avg(pmemmax) as pmemmax,
       avg(memsize) as memsize, avg(memresid) as memresid,
       avg(memshare) as memshare, avg(cpu_elapsed) as cpu_elapsed,
       avg(cpu_currpct) as cpu_currpct, array_to_string(iterators_array_accum(distinct phase), ',') as phase, 
       sum(rows_out) as rows_out, sum(rows_out_est) as rows_out_est,
       case when avg(cpu_elapsed) <= 0.001 THEN 0 else
          (stddev(cpu_elapsed)/avg(cpu_elapsed))*100 end as skew_cpu,
       case when avg(rows_out) <= 0.001 THEN 0 else
          (stddev(rows_out)/avg(rows_out))*100 end as skew_rows,
       min(m0_name)||'|'||min(m0_unit)||'|'||avg(m0_val)||'|'||avg(m0_est) as m0,
       min(m1_name)||'|'||min(m1_unit)||'|'||avg(m1_val)||'|'||avg(m1_est) as m1,
       min(m2_name)||'|'||min(m2_unit)||'|'||avg(m2_val)||'|'||avg(m2_est) as m2,
       min(m3_name)||'|'||min(m3_unit)||'|'||avg(m3_val)||'|'||avg(m3_est) as m3,
       min(m4_name)||'|'||min(m4_unit)||'|'||avg(m4_val)||'|'||avg(m4_est) as m4,
       min(m5_name)||'|'||min(m5_unit)||'|'||avg(m5_val)||'|'||avg(m5_est) as m5,
       min(m6_name)||'|'||min(m6_unit)||'|'||avg(m6_val)||'|'||avg(m6_est) as m6,
       min(m7_name)||'|'||min(m7_unit)||'|'||avg(m7_val)||'|'||avg(m7_est) as m7,
       min(m8_name)||'|'||min(m8_unit)||'|'||avg(m8_val)||'|'||avg(m8_est) as m8,
       min(m9_name)||'|'||min(m9_unit)||'|'||avg(m9_val)||'|'||avg(m9_est) as m9,
       min(m10_name)||'|'||min(m10_unit)||'|'||avg(m10_val)||'|'||avg(m10_est) as m10,
       min(m11_name)||'|'||min(m11_unit)||'|'||avg(m11_val)||'|'||avg(m11_est) as m11,
       min(m12_name)||'|'||min(m12_unit)||'|'||avg(m12_val)||'|'||avg(m12_est) as m12,
       min(m13_name)||'|'||min(m13_unit)||'|'||avg(m13_val)||'|'||avg(m13_est) as m13,
       min(m14_name)||'|'||min(m14_unit)||'|'||avg(m14_val)||'|'||avg(m14_est) as m14,
       min(m15_name)||'|'||min(m15_unit)||'|'||avg(m15_val)||'|'||avg(m15_est) as m15,
       min(t0_name)||'|'||min(t0_val) as t0
       from %s""" % table
    query += "\nwhere tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    if start != '' and end != '':
        query += " and ctime>='%s' and ctime<='%s'" % (start, end)
    query += "\ngroup by tmid, ssid, ccnt, nid"

    return query


def queryplan_now(tmid, ssid, ccnt, username, dbuser, dbpass):
    # If not superuser, check that username matches
    if username != '':
        q_username = queryplan_get_username_now(tmid, ssid, ccnt, dbuser, dbpass)
        if q_username != username:
            return _queryplan_consolidate(rows)
            
    query1 = queryplan_sql("iterators_now", tmid, ssid, ccnt)
    query = query1 + " order by pnid, nid"

    rows = execute_query(query, dbuser, dbpass)

    return _queryplan_consolidate(rows)



def queryplan_get_username_history(tmid, ssid, ccnt, start, end, username, password):
    query_fragment = "select username"
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s'" % (tmid, ssid, ccnt)
    if start != '' and end != '':
        filter += " and ctime>='%s' and ctime<='%s'" % (start, end)
    query1 = "%s from queries_history %s" % (query_fragment, filter)
    query2 = "%s from queries_tail %s" % (query_fragment, filter)
    query = "(%s) union all (%s) limit 1" % (query1, query2)
    
    result = ''
    try:
        r = execute_single_value_query(query, username, password)
        result = '%s' % r
    except:
        log.msg('error during query: %s' % query)
        raise_db_error('Exception during query: %s' % query)
    return result


def queryplan_history(tmid, ssid, ccnt, username, start, end, dbuser, dbpass):
    # If not superuser, check that username matches
    if username != '':
        q_username = queryplan_get_username_history(tmid, ssid, ccnt, start, end, dbuser, dbpass)
        if q_username != username:
            return _queryplan_consolidate(rows)
            
    query1 = queryplan_sql("iterators_history", tmid, ssid, ccnt, start, end)
    query2 = queryplan_sql("iterators_tail", tmid, ssid, ccnt)
    query = "(%s) union all (%s) order by pnid, nid" % (query1, query2)

    rows = execute_query(query, dbuser, dbpass)

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
                'm15_name', 'm15_unit', 'm15_val', 'm15_est',
                't0_name', 't0_val' ]

    rows = []
    try:
        f = open(filename, 'r')
    except:
        log.msg('could not open iterators_now file: %s' % filename)
        raise GPDBError, 'Real-time iterators_now file unavailable'
        return ''

    try:
        for line in f:

            columns = {} # dict to contain the values of each column
            entry = line.split('|')
            # positional matching. if cnames is modified, the index
            # might be affected.
            if entry[1] != tmid:
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[2] != ssid:
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif entry[3] != ccnt:
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif segid != '' and entry[4] != segid:
                continue
            # positional matching. if cnames is modified, the index
            # might be affected.
            elif nid != '' and entry[6] != nid:
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
                'm15_name', 'm15_unit', 'm15_val', 'm15_est',
                't0_name', 't0_val' ]

    query_fragment = "select ctime"
    for column in columns:
        query_fragment += ", %s" % column

    return query_fragment


# returns iterator data across all segments.
def queryiterator(tmid, ssid, ccnt, nid, dbuser, dbpass):
    query_fragment = iterator_sql()
    filter = "where tmid='%s' and ssid='%s' and ccnt='%s' and nid='%s'" % (tmid, ssid, ccnt, nid)

    query1 = "%s from iterators_history %s" % (query_fragment, filter)
    query2 = "%s from iterators_tail %s" % (query_fragment, filter)
    query = "%s\nunion all\n%s" % (query1, query2)

    rows = execute_query(query, dbuser, dbpass)

    # query real-time information.
    segid = ''
    for r in rt_queryiterators(tmid, ssid, ccnt, segid, nid):
        rows.append(r)

    return _iterator_consolidate(rows)



def database_now(remote, dbuser, dbpass):
    if remote:
        interval_code = 7
        query = database_sql('database_now', '', interval_code)
        rows = execute_query(query, dbuser, dbpass)
    else:
        rows = database_file()

    return _database_consolidate(rows)


def database_file():
    filename = "%s/database_now.dat" % path

    # ordering is important to match the order in the file
    cnames = [  'sample_time', 'queries_total', 
                'queries_running', 'queries_queued']

    rows = []

    try:
        f = open(filename, 'r')
    except:
        log.msg('could not open database_now file: %s' % filename)
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
                "date_trunc('year', ctime)+(((trunc(date_part('month', ctime)/6)*6)::text||' months')::interval)",
                "ctime"]


    query_fragment = "select %s as sample_time" % itvl_sql[interval_code]
    for column in columns:
        query_fragment += ", avg(%s) as %s" % (column, column)
        if interval_code != 7:
            query_fragment += ", min(%s) as %s_min" % (column, column)
            query_fragment += ", max(%s) as %s_max" % (column, column)

    query_fragment += " from %s %s" % (table, filter)
    query_fragment += " group by sample_time"
    return query_fragment


def database_history(stime, etime, interval_code, dbuser, dbpass):
    if etime == 'NOW':
        filter = "where ctime >= '%s'" % stime
    else:
        filter = "where '%s' <= ctime and ctime < '%s'" % (stime, etime)

    query1 = database_sql('database_history', filter, interval_code)
    query2 = database_sql('database_tail', filter, interval_code)
    query = '%s union all %s' % (query1, query2)

    rows = execute_query(query, dbuser, dbpass)

    return _database_consolidate(rows)


def database_state(dbuser, dbpass):

    dbstatus_normal = 'NORMAL'
    dbstatus_degraded = 'DEGRADED'
    dbstatus_down = 'DOWN'
    dbstatus_unbalanced = 'UNBALANCED'

    degradeState_query =  "select count(*) from gp_segment_configuration where mode <> 's'"
    unbalancedState_query = "select count(*) from gp_segment_configuration where role <> preferred_role"

    try:
        result = execute_query(degradeState_query, dbuser, dbpass)

        if result[0]['count'] != 0:
            return dbstatus_degraded

    except Exception, e:
        log.msg('GPDB error on query (%s) (%s)' % (degradeState_query, e.__str__()))
        dbstatus = 'DOWN'
        return dbstatus_down
    except:
        log.msg('GPDB error on query (%s)' % (degradeState_query))
        dbstatus = 'DOWN'
        return dbstatus_down

    try:
        result = execute_query(unbalancedState_query, dbuser, dbpass)
        if result[0]['count'] != 0:
            return dbstatus_unbalanced
    except Exception, e:
        log.msg('GPDB error on query (%s) (%s)' % (unbalancedState_query, e.__str__()))
        return dbstatus_down
    except Exception, e:
        log.msg('GPDB error on query (%s)' % (unbalancedState_query))
        return dbstatus_down

    return dbstatus_normal

def readLastUpdateTimeForHealthFromDisk():
    retval = 0
    filename = "%s/snmp/lastreport.txt" % (path)
    try:
        f = open(filename, 'r')
    except:
        return 0

    try:
        for line in f:
            line = line.strip()
            cols = line.split("|")
            if len(cols) == 2:
                retval = int(cols[1])
            break
    finally:
        f.close()

    return retval

class HealthInfo:
    def __init__(self):
        self.lastreadfromdisk = 0
        self.updatetime = 0
        self.table = None # hash (k: categories v: list) ... each item in list is a tuple (hostname, status)

cachedHealthInfo = HealthInfo() 

def health():

    # cache values for 10 seconds to avoid repeated access to filesystem
    if (time.time() - cachedHealthInfo.lastreadfromdisk) > 10:
        cachedHealthInfo.lastreadfromdisk = time.time()
        cachedHealthInfo.updatetime = readLastUpdateTimeForHealthFromDisk()
        cachedHealthInfo.table = dict()

        filename = "%s/snmp/hostlistreport.txt" % (path)
        try:
            f = open(filename, 'r')
        except:
            log.msg('could not open health file: %s' % filename)
            raise GPDBError, 'health data unavailable'

        try:
            for line in f:
                line = line.strip()
                cols = line.split("|")
                if len(cols) != 3:
                    continue
        
                category = cols[0]

                if category not in cachedHealthInfo.table:
                    cachedHealthInfo.table[category] = list()

                # add (hostname, status_
                cachedHealthInfo.table[category].append( (cols[1], cols[2]) )

        except Exception, e:
            log.msg('error reading from health file: %s' % filename)
            log.msg(e.__str__())
            raise GPDBError, 'health status unavailable'
        finally:
            f.close()

        cachedHealthInfo.lastreadfromdisk = time.time()

    return (cachedHealthInfo.updatetime, cachedHealthInfo.table)
    

def healthdetails(hostname):

    results = list()
    filename = "%s/snmp/snmp.host.%s.txt" % (path, hostname)
    try:
        f = open(filename, 'r')
    except:
        log.msg('could not open health details file: %s' % filename)
        raise GPDBError, 'health details for %s unavailable' % hostname

    try:
        for line in f:
            line = line.strip()
            cols = line.split("|")
            if len(cols) != 8:
                continue
            results.append(cols)
    except:
        log.msg('error reading from health details file: %s' % filename)
        raise GPDBError, 'health details for %s unavailable' % hostname
    finally:
        f.close()

    return results





