"""
Session Management
(from web.py)
"""

import sys, os, time, datetime, random, base64
try:
    import cPickle as pickle
except ImportError:
    import pickle
try:
    import hashlib
    sha1 = hashlib.sha1
except ImportError:
    import sha
    sha1 = sha.new

import utils
import webapi as web

__all__ = [
    'Session', 'SessionExpired',
    'Store', 'DiskStore', 'DBStore',
]

web.config.session_parameters = utils.storage({
    'cookie_name': 'webpy_session_id',
    'cookie_domain': None,
    'timeout': 86400, #24 * 60 * 60, # 24 hours in seconds
    'ignore_expiry': True,
    'ignore_change_ip': True,
    'secret_key': 'fLjUfxqXtfNoIldA0A0J',
    'expired_message': 'Session expired',
})

class SessionExpired(web.HTTPError): 
    def __init__(self, message):
        web.HTTPError.__init__(self, '200 OK', {}, data=message)

class Session(utils.ThreadedDict):
    """Session management for web.py
    """

    def __init__(self, app, store, initializer=None):
        self.__dict__['store'] = store
        self.__dict__['_initializer'] = initializer
        self.__dict__['_last_cleanup_time'] = 0
        
        # GREENPLUM - to allow multiple instances to run
        if initializer.has_key('gpperfmon_instance_name'):
            web.config.session_parameters['cookie_name'] = 'gpperfmon_instance_%s' % initializer['gpperfmon_instance_name']
        # END GREENPLUM CHANGE


        self.__dict__['_config'] = utils.storage(web.config.session_parameters)

        if app:
            app.add_processor(self._processor)

    def _processor(self, handler):
        """Application processor to setup session for every request"""
        self._cleanup()
        self._load()

        try:
            return handler()
        finally:
            self._save()

    def _load(self):
        """Load the session from the store, by the id from cookie"""
        cookie_name = self._config.cookie_name
        cookie_domain = self._config.cookie_domain
        self.session_id = web.cookies().get(cookie_name)
        
        self._check_expiry()
        if self.session_id:
            d = self.store[self.session_id]
            self.update(d)
            self._validate_ip()
        
        if not self.session_id:
            self.session_id = self._generate_session_id()

            if self._initializer:
                if isinstance(self._initializer, dict):
                    self.update(self._initializer)
                elif hasattr(self._initializer, '__call__'):
                    self._initializer()
 
        self.ip = web.ctx.ip

    def _check_expiry(self):
        # check for expiry
        if self.session_id and self.session_id not in self.store:
            if self._config.ignore_expiry:
                self.session_id = None
            else:
                return self.expired()

    def _validate_ip(self):
        # check for change of IP
        if self.session_id and self.get('ip', None) != web.ctx.ip:
            if not self._config.ignore_change_ip:
               return self.expired() 
    
    def _save(self):
        cookie_name = self._config.cookie_name
        cookie_domain = self._config.cookie_domain
        if not self.get('_killed'):
            web.setcookie(cookie_name, self.session_id, domain=cookie_domain)
            self.store[self.session_id] = dict(self)
        else:
            web.setcookie(cookie_name, self.session_id, expires=-1, domain=cookie_domain)
    
    def _generate_session_id(self):
        """Generate a random id for session"""

        while True:
            rand = os.urandom(16)
            now = time.time()
            secret_key = self._config.secret_key
            session_id = sha1("%s%s%s%s" %(rand, now, utils.safestr(web.ctx.ip), secret_key))
            session_id = session_id.hexdigest()
            if session_id not in self.store:
                break
        return session_id
        
    def _cleanup(self):
        """Cleanup the stored sessions"""
        current_time = time.time()
        timeout = self._config.timeout
        if current_time - self._last_cleanup_time > timeout:
            self.store.cleanup(timeout)
            self.__dict__['_last_cleanup_time'] = current_time

    def expired(self):
        """Called when an expired session is atime"""
        raise SessionExpired(self._config.expired_message)
 
    def kill(self):
        """Kill the session, make it no longer available"""
        del self.store[self.session_id]
        self._killed = True

    def getid(self):
        return self.session_id


class Store:
    """Base class for session stores"""

    def __contains__(self, key):
        raise NotImplemented

    def __getitem__(self, key):
        raise NotImplemented

    def __setitem__(self, key, value):
        raise NotImplemented

    def cleanup(self, timeout):
        """removes all the expired sessions"""
        raise NotImplemented

    def encode(self, session_dict):
        """encodes session dict as a string"""
        pickled = pickle.dumps(session_dict)
        return base64.encodestring(pickled)

    def decode(self, session_data):
        """decodes the data to get back the session dict """
        pickled = base64.decodestring(session_data)
        return pickle.loads(pickled)

class DiskStore(Store):
    """Store for saving a session on disk

        >>> import tempfile
        >>> root = tempfile.mkdtemp()
        >>> s = DiskStore(root)
        >>> s['a'] = 'foo'
        >>> s['a']
        'foo'
        >>> time.sleep(0.01)
        >>> s.cleanup(0.01)
        >>> s['a']
        Traceback (most recent call last):
            ...
        KeyError: 'a'
    """
    SESSION_PREFIX = 'session-'
    LOCK_SUFFIX = '.lock'

    def __init__(self, root):
        # if the storage root doesn't exists, create it.
        if not os.path.exists(root):
            os.mkdir(root)
        self.root = root
    
    def __contains__(self, key):
        path = os.path.join(self.root, key)
        return os.path.exists(path)

    def __getitem__(self, key):
        path = os.path.join(self.root, key)
        if os.path.exists(path): 
            #GREENPLUM
            self.acquire_lock(key)
            #GREENPLUM
            pickled = open(path).read()
            #GREENPLUM
            self.release_lock(key)
            #GREENPLUM
            return self.decode(pickled)
        else:
            raise KeyError, key

    def __setitem__(self, key, value):
        pickled = self.encode(value)    
        path = os.path.join(self.root, key)
        #GREENPLUM
        self.acquire_lock(key)
        #GREENPLUM
        try:
            f = open(path, 'w')
            try:
                f.write(pickled)
            finally: 
                f.close()
        except IOError:
            pass
        #GREENPLUM
        finally:
            self.release_lock(key)
        #GREENPLUM

    def __delitem__(self, key):
        path = os.path.join(self.root, key)
        if os.path.exists(path):
            os.remove(path)
    
    def cleanup(self, timeout):
        now = time.time()
        for f in os.listdir(self.root):
            path = os.path.join(self.root, f)
            atime = os.stat(path).st_atime
            if now - atime > timeout :
                os.remove(path)

    # GREENPLUM  additions to provide session locking support     
    # BEGINS HERE 

    def _get_file_path(self, key):
        return os.path.join(self.root, self.SESSION_PREFIX + key)

    def acquire_lock(self, key):
        path = self._get_file_path(key)
        path += self.LOCK_SUFFIX
        while True:
            try:
                lockfd = os.open(path, os.O_CREAT|os.O_WRONLY|os.O_EXCL)
            except OSError:
                time.sleep(0.1)
            else:
                os.close(lockfd)
                break
        self.locked = True

    def release_lock(self, key):
        path = self._get_file_path(key)
        os.unlink(path + self.LOCK_SUFFIX)
        self.locked = False

    # ENDS HERE 

class DBStore(Store):
    """Store for saving a session in database
    Needs a table with the following columns:

        session_id CHAR(128) UNIQUE NOT NULL,
        atime DATETIME NOT NULL default current_timestamp,
        data TEXT
    """
    def __init__(self, db, table_name):
        self.db = db
        self.table = table_name
    
    def __contains__(self, key):
        data = self.db.select(self.table, where="session_id=$key", vars=locals())
        return bool(list(data)) 

    def __getitem__(self, key):
        now = datetime.datetime.now()
        try:
            s = self.db.select(self.table, where="session_id=$key", vars=locals())[0]
            self.db.update(self.table, where="session_id=$key", atime=now, vars=locals())
        except IndexError:
            raise KeyError
        else:
            return self.decode(s.data)

    def __setitem__(self, key, value):
        pickled = self.encode(value)
        now = datetime.datetime.now()
        if key in self:
            self.db.update(self.table, where="session_id=$key", data=pickled, vars=locals())
        else:
            self.db.insert(self.table, False, session_id=key, data=pickled )
                
    def __delitem__(self, key):
        self.db.delete(self.table, where="session_id=$key", vars=locals())

    def cleanup(self, timeout):
        timeout = datetime.timedelta(timeout/(24.0*60*60)) #timedelta takes numdays as arg
        last_allowed_time = datetime.datetime.now() - timeout
        self.db.delete(self.table, where="$last_allowed_time > atime", vars=locals())

if __name__ == '__main__' :
    import doctest
    doctest.testmod()
