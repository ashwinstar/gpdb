import os, sys, datetime

sys.path.append(os.path.join(sys.path[0], 'lib'))
import pg8000 

pguser = 'gpmon'
pgdatabase = os.environ.get('PGDATABASE', 'gpmon_2')
pgpass = os.environ.get('PGPASSWORD', '')
pgport = os.environ.get('PGPORT', '5432')
pgport = int(pgport)
pghost = os.environ.get('PGHOST', 'localhost')
conn = pg8000.Connection(user=pguser, password=pgpass, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)

def start_date(): 
	conn.execute('select min(ctime) from metrics_history') 
	for r in conn.iterate_dict():
			time = r['min']
	return time

def hosts(): 
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
        ckeys = x.keys(); ckeys.sort()

    tkeys = td.keys(); tkeys.sort()
    hkeys = hd.keys(); hkeys.sort()
    return (tkeys, hkeys, ckeys, tab)


def metrics_now(host):
    if host:
        conn.execute("select * from metrics_now where hostname='%s'" % host)
    else:
        conn.execute('select * from metrics_now');
    
    rows = [r for r in conn.iterate_dict()]
    return _metrics_consolidate(rows)


def metrics_upto_now(host, stime):
    filter = "where ctime >= '%s'" % stime
    if host:
        filter = filter + " and hostname='%s'" % host

    query = '''
select * from metrics_history %s
union all 
select * from metrics_tail %s
union all 
select * from metrics_now  %s
''' % (filter, filter, filter)

    sys.stderr.write('%s\n' % query)
    conn.execute(query)
    rows = [r for r in conn.iterate_dict()]
    return _metrics_consolidate(rows)

def metrics_history(host, stime, etime):
    filter = "where '%s' <= ctime and ctime <= '%s'" % (stime, etime)
    if host:
        filter = filter + " and hostname='%s'" % host

    query = '''
select * from metrics_history %s
union all 
select * from metrics_tail %s
''' % (filter*3)

    sys.stderr.write('%s\n' % query)
    conn.execute(query)
    return _metrics_consolidate(row)
