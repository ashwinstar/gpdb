import os, sys

sys.path.append(os.path.join(sys.path[0], 'lib'))
import pg8000 

pguser = 'gpmon'
pgdatabase = os.environ.get('PGDATABASE', 'gpmon_2')
pgpass = os.environ.get('PGPASSWORD', '')
pgport = os.environ.get('PGPORT', '5432')
pgport = int(pgport)
pghost = os.environ.get('PGHOST', 'localhost')
conn = pg8000.Connection(user=pguser, password=pgpass, host=pghost, port=pgport, database=pgdatabase, socket_timeout=8*60*60)

def metrics_now(host):
    if host:
        conn.execute("select * from metrics_now where hostname='%s'" % host)
    else:
        conn.execute('select * from metrics_now order by hostname');
    return conn.iterate_dict()

