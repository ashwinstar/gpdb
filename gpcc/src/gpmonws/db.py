import os, sys, datetime

sys.path.append(os.path.join(sys.path[0], 'lib'))
import pg8000 

pguser = 'gpmon'
pgdatabase = os.environ.get('PGDATABASE', 'gpmon_2')
pgpass = os.environ.get('PGPASSWORD', '')
pgport = os.environ.get('PGPORT', '5432')
pgport = int(pgport)
pghost = os.environ.get('PGHOST', 'localhost')

try:
    conn = pg8000.Connection(user=pguser, password=pgpass, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)
except pg8000.errors.InterfaceError:
    sys.stderr.write('GPDB connection failure: database unavailable\n')
    sys.exit()
except pg8000.errors.InternalError:
    sys.stderr.write('GPDB connection failure: invalid user or password\n')
    sys.exit()
except:
    sys.stderr.write('GPDB connection failure: %s\n' % sys.exc_info()[0])
    conn = None

def getrole(username, password):
    res = (False, False)
    try:
        auth_conn = pg8000.Connection(user=username, password=password, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)

        auth_conn.execute('select rolsuper from pg_roles where rolname = current_user')
        for r in auth_conn.iterate_dict():
            res = (True, r['rolsuper'])
            
        auth_conn.close()
    except:
        sys.stderr.write('Exception connecting')

    return res

def start_date(): 
    # check  connection to database 
    if conn == None:
        return None

    conn.execute('select min(ctime) from metrics_history') 
    for r in conn.iterate_dict():
        return r['min']
    return '2008-01-01 00:00:00'

def hosts(): 
    # check  connection to database 
    if conn == None:
        return None

    conn.execute('select distinct(hostname) from metrics_now') 
    rows = []
    for r in conn.iterate_dict():
        rows.append(r['hostname'])

    return rows


def _metrics_consolidate(rows):
    td = {}   #time dimension
    hd = {}   #host dimension
    tab = {}  #table, key = (host, time)

    x = None
    for r in rows:
        (h, t) = (r['hostname'], r['ctime'])
        td[t] = 1; hd[h] = 1
        tab[(h,t)] = r
        x = r

    ckeys = None
    if x: 
        ckeys = filter(lambda i: i != 'hostname', x.keys())
	ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    hkeys = hd.keys(); hkeys.sort()
    return (tkeys, hkeys, ckeys, tab)


def build_query_string(table, filter, interval):
    agg_func = 'avg'

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


def metrics_next(prev, host):
    # check  connection to database 
    if conn == None:
        return None

    if host:
        conn.execute("select *, now() as curtime from metrics_now where hostname='%s'" % host)
    else:
        conn.execute('select *, now() as curtime from metrics_now');

    rows = [r for r in conn.iterate_dict()]
    ctime = rows['ctime']
    if ctime != prev:
        return _metrics_consolidate(rows)

    try:
	ctime = datetime.datetime.strptime(ctime, '%Y-%m-%d %H:%M:%S')
	curtime = datetime.datetime.strptime(rows['curtime'], '%Y-%m-%d %H:%M:%S')
    except ValueError:
        return _metrics_consolidate(rows)

    


def metrics_now(host):
    # check  connection to database 
    if conn == None:
        return None

    if host:
        conn.execute("select * from metrics_now where hostname='%s'" % host)
    else:
        conn.execute('select * from metrics_now');
    
    rows = [r for r in conn.iterate_dict()]
    return _metrics_consolidate(rows)




def metrics_upto_now(host, stime, interval):
    filter = "where ctime >= '%s'" % stime
    if host:
        filter = filter + " and hostname='%s'" % host

    query1 = build_query_string('metrics_history', filter, interval)
    query2 = build_query_string('metrics_tail', filter, interval)
    query3 = build_query_string('metrics_now', filter, interval)
    query = "%s\nunion all\n%s\nunion all\n%s" % (query1, query2, query3)

    conn.execute(query)
    rows = [r for r in conn.iterate_dict()]
    return _metrics_consolidate(rows)

def metrics_history(host, stime, etime, interval):
    filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)
    if host:
        filter = filter + " and hostname='%s'" % host

    query1 = build_query_string('metrics_history', filter, interval)
    query2 = build_query_string('metrics_tail', filter, interval)
    query = "%s\nunion all\n%s" % (query1, query2)

    conn.execute(query)
    rows = [r for r in conn.iterate_dict()]
    return _metrics_consolidate(rows)



def queries_now():
    conn.execute('select * from queries_now')
    rows = [r for r in conn.iterate_dict()]
    for r in rows:
        for k in r:
            r[k] = str(r[k])
            if k == 'query_text':
                if len(r[k]) > 50: r[k] = r[k][:50]
    return _metrics_consolidate(rows)
