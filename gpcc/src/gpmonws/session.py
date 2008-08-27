import web, tempfile

STORE = web.session.DiskStore(tempfile.mkdtemp())
web.config.session_parameters.timeout = 1800


def get(role=None):
    s = web.ctx.session
    s.start()
    try:
        user = s['user']
    except KeyError:
        s['user'] = None

    # role not used for now
    return s

def destroy():
    s = get()
    s['user'] = None
    s.destroy()
