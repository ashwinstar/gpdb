import os, sys

progname = os.path.split(sys.argv[0])[-1]


if sys.version_info < (2, 3, 5):
    sys.exit(
'''Error: %s is supported on Python versions 2.3.5 or greater
Please upgrade python installed on this machine.''' % progname)


################
def cli_help():
    help_path = os.path.join(sys.path[0], '..', 'docs', 'cli_help', progname + '_help');
    f = None
    try:
        try:
            f = open(help_path);
            return f.read(-1)
        except:
            return ''
    finally:
        if f: f.close()


################
def parse_port(val): 
    try: return int(val)
    except ValueError: return 0


################
def parse_hostline(line, defuser, defport):
    '''Parse line into (user, host, port) tuple'''
    try:
	user = None
	host = None
	port = None
	t = line.strip().split('@')
	cnt = len(t)
	if t[0][0] == '#': return None
        if cnt == 1: 
	    host = t[0]
	else:
	    user = t[0]
	    host = t[1]

	t = host.split(':')
	if len(t) > 1:
	    host = t[0]
	    port = int(t[1])

        if defuser and user == None: user = defuser
        if defport and port == None: port = defport
	    
	return (user, host, port)

    except ValueError: pass
    except IndexError: pass
    return None

#############
def read_hostfile(fname, user, port):
    retList = []
    if not fname: return retList
    f = None; lno = 0; host = {}
    try:
	try:
	    f = open(fname, 'r')
	    for line in f:
		lno = lno + 1
		h = parse_hostline(line, user, port)
		if not h: continue
		key = '%s@%s' % (h[0], h[1])
		host[key] = h
	    for n in host: retList.append(host[n])
	except IOError:
	    sys.exit('[ERROR] cannot read host file ' + fname 
		     + (lno > 0 and (' (line %d)' % lno) or ''))
    finally:
	if f: f.close()

    return retList


#############
def parse_hostlines_hostfile(opt_h, opt_f, user, port):
    tab = []
    for line in opt_h:
        h = parse_hostline(line, user, port)
        if h: tab.append(h)
        else: 
            sys.exit('Error: invalid host name - %s' % line)

    tab.extend(read_hostfile(opt_f, user, port))
    return tab


#############
def ssh_prefix(user, host, port):
    ssh = 'ssh -o "BatchMode yes" -o "StrictHostKeyChecking no"'
    if port:
        ssh = ssh + ' -p %d' % port
    if user:
        ssh = ssh + ' -l %s' % user
    ssh = ssh + ' ' + host
    return ssh


#############
def scp_prefix(port):
    scp = 'scp -o "BatchMode yes" -o "StrictHostKeyChecking no"'
    if port:
        scp = scp + ' -P %d' % port
    return scp


