import os
import gzip
import getpass
import shutil
import socket

# TODO: trim down the wildcard imports to only what's necessary
from gppylib import gplog
from gppylib.db import dbconn
from gppylib.db.dbconn import execSQL, execSQLForSingleton
from gppylib.gparray import GpArray
from gppylib.mainUtils import ExceptionNoStackTraceNeeded
from gppylib.commands.base import WorkerPool, Command
from gppylib.commands.gp import Psql
from gppylib.commands.unix import Scp
from gppylib.operations import Operation
from gppylib.operations.utils import RemoteOperation, ParallelOperation
from gppylib.operations.unix import CheckFile, CheckRemoteFile, CheckRemoteDir, MakeRemoteDir, RemoveFile, RemoveRemoteFile

"""
TODO: partial restore. In 4.x, dump will only occur on primaries. 
So, after a dump, dump files must be pushed to mirrors. (This is a task for gpcrondump.)
"""

""" TODO: centralize logging """
logger = gplog.get_default_logger()

WARN_MARK = '<<<<<'
DUMP_DIR = 'db_dumps'
DBDUMP_PREFIX = 'gp_dump_'
MASTER_DBDUMP_PREFIX = 'gp_dump_1_1_'
GLOBAL_PREFIX = 'gp_global_1_1_'
CREATEDB_PREFIX = 'gp_cdatabase_1_1_'

# TODO: use CLI-agnostic custom exceptions instead of ExceptionNoStackTraceNeeded

class RestoreDatabase(Operation):
    def __init__(self, restore_timestamp, no_analyze, drop_db, restore_global, master_datadir, master_port, ddboost): 
        self.restore_timestamp = restore_timestamp
        self.no_analyze = no_analyze
        self.drop_db = drop_db
        self.restore_global = restore_global
        self.master_datadir = master_datadir
        self.master_port = master_port
        self.ddboost = ddboost
    def execute(self): 
        (restore_timestamp, restore_db, compress) = ValidateRestoreDatabase(restore_timestamp = self.restore_timestamp,
                                                                            master_datadir = self.master_datadir,
                                                                            master_port = self.master_port,
                                                                            ddboost = self.ddboost).run()
        if self.drop_db:
            self._process_createdb(restore_timestamp, restore_db, self.master_datadir, self.master_port)
        
        if self.restore_global:
            self._restore_global(restore_timestamp, self.master_datadir)

        restore_line = self._build_restore_line(restore_timestamp, restore_db, compress, self.master_port, self.ddboost)
        logger.info(restore_line)
        Command('Invoking gp_restore', restore_line).run()

        if not self.no_analyze:
            self._analyze(restore_db, self.master_port)
    def _analyze(self, restore_db, master_port):
        conn = None
        logger.info('Commencing analyze of %s database, please wait' % restore_db)
        try:
            dburl = dbconn.DbURL(port=master_port, dbname=restore_db)
            conn = dbconn.connect(dburl)
            execSQL(conn, 'analyze')
        except Exception, e:
            logger.warn('Issue with analyze of %s database' % restore_db)
        else:
            logger.info('Analyze of %s completed without error' % restore_db)
        finally:
            if conn is not None:
                conn.close()
    def _restore_global(self, restore_timestamp, master_datadir): 
        logger.info('Commencing restore of global objects')
        global_file = os.path.join(master_datadir, DUMP_DIR, restore_timestamp[0:8], "%s%s" % (GLOBAL_PREFIX, restore_timestamp))
        if not CheckFile(global_file).run():
            logger.warn('Unable to locate %s%s file in dump set' % (GLOBAL_PREFIX, restore_timestamp))
            return
        Psql('Invoking global dump', filename=global_file).run(validateAfter=True)
    def _process_createdb(self, restore_timestamp, restore_db, master_datadir, master_port): 
        conn = None
        try:
            dburl = dbconn.DbURL(port=master_port)
            conn = dbconn.connect(dburl)
            count = execSQLForSingleton(conn, "select count(*) from pg_database where datname='%s';" % restore_db)

            if count == 1:
                logger.info("Dropping database %s" % restore_db)
                try:
                    cursor=conn.cursor()
                    cursor.execute("commit")                          # hack to move drop stmt out of implied transaction
                    cursor.execute("drop database %s" % restore_db)
                    cursor.close()
                except Exception, e:
                    logger.exception("Could not create database %s" % restore_db)
                    raise ExceptionNoStackTraceNeeded('Failed to drop database %s' % restore_db)
                else:
                    logger.info('Dropped database %s' % restore_db)
        finally:
            if conn is not None:
                conn.close()

        createdb_file = os.path.join(master_datadir, DUMP_DIR, restore_timestamp[0:8], "%s%s" % (CREATEDB_PREFIX, restore_timestamp))
        logger.info('Invoking %s' % createdb_file)
        Psql('Invoking schema dump', filename=createdb_file).run(validateAfter=True)
    def _build_restore_line(self, restore_timestamp, restore_db, compress, master_port, ddboost): 
        user = getpass.getuser()
        hostname = socket.gethostname()    # TODO: can this just be localhost? bash was using `hostname`
        path = os.path.join(DUMP_DIR, restore_timestamp[0:8])
        restore_line = "gp_restore -i -h %s -p %s -U %s --gp-d=%s --gp-i" % (hostname, master_port, user, path)
        restore_line += " --gp-k=%s --gp-r=%s --gp-l=p" % (restore_timestamp, path)
        if compress:
            restore_line += " --gp-c"
        restore_line += " -d %s" % restore_db
        if self.ddboost is not None:
            restore_line += " --gp-ddb-host=%s" % self.ddboost['host']
            restore_line += " --gp-ddb-user=%s" % self.ddboost['user']
            restore_line += " --gp-ddb-passwd=%s" % self.ddboost['passwd']
        return restore_line



class ValidateRestoreDatabase(Operation):
    """ TODO: add other checks. check for _process_createdb? """
    def __init__(self, restore_timestamp, master_datadir, master_port, ddboost):
        self.restore_timestamp = restore_timestamp
        self.master_datadir = master_datadir
        self.master_port = master_port
        self.ddboost = ddboost
    def execute(self):
        (restore_timestamp, restore_db, compress) = ValidateTimestamp(self.restore_timestamp, self.master_datadir, self.ddboost).run()   
        if self.ddboost is None:
            ValidateSegments(restore_timestamp, compress, self.master_port).run()
        return (restore_timestamp, restore_db, compress)



class ValidateTimestamp(Operation):
    def __init__(self, candidate_timestamp, master_datadir, ddboost=None):
        self.master_datadir = master_datadir
        self.candidate_timestamp = candidate_timestamp
        self.ddboost = ddboost
    def execute(self):
        path = os.path.join(self.master_datadir, DUMP_DIR, self.candidate_timestamp[0:8])
        createdb_file = os.path.join(path, "%s%s" % (CREATEDB_PREFIX, self.candidate_timestamp))
        if not CheckFile(createdb_file).run():
            raise ExceptionNoStackTraceNeeded("Dump file %s%s does not exist on Master" % (CREATEDB_PREFIX, self.candidate_timestamp))
        restore_db = GetDbName(createdb_file).run()
        if self.ddboost is None:
            compressed_file = os.path.join(path, "%s%s.gz" % (MASTER_DBDUMP_PREFIX, self.candidate_timestamp))
            compress = CheckFile(compressed_file).run()
        else:
            compress = False
        return (self.candidate_timestamp, restore_db, compress)



class ValidateSegments(Operation):
    def __init__(self, restore_timestamp, compress, master_port):
        self.restore_timestamp = restore_timestamp
        self.compress = compress
        self.master_port = master_port
    def execute(self): 
        """ TODO: Improve with grouping by host and ParallelOperation dispatch. """
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)   
        primaries = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]
        dump_count = 0
        for seg in primaries:
            if seg.isSegmentDown():
                """ Why must every Segment function have the word Segment in it ?! """
                raise ExceptionNoStackTraceNeeded("Host %s dir %s dbid %d marked as invalid" % (seg.getSegmentHostName(), seg.getSegmentDataDirectory(), seg.getSegmentDbId()))

            path = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, self.restore_timestamp[0:8])
            host = seg.getSegmentHostName()
            path = os.path.join(path, "%s0_%d_%s" % (DBDUMP_PREFIX, seg.getSegmentDbId(), self.restore_timestamp))
            if self.compress:
                path += ".gz"
            exists = CheckRemoteFile(path, host).run()
            if not exists:
                raise ExceptionNoStackTraceNeeded("No dump file on %s at %s" % (seg.getSegmentHostName(), path))



class RestoreTables(Operation):
    """ If this seems like a hacky composition of RestoreDatabase, you're absolutely correct. """
    def __init__(self, restore_timestamp, restore_tables, no_analyze, keep_dump_files, batch_default, master_datadir, master_port, ddboost): 
        self.restore_timestamp = restore_timestamp
        self.restore_tables = restore_tables
        self.batch_default = batch_default
        self.master_datadir = master_datadir
        self.master_port = master_port
        self.no_analyze = no_analyze
        self.keep_dump_files = keep_dump_files
        self.ddboost = ddboost
    def execute(self):
        (restore_timestamp, restore_db, compress) = ValidateTimestamp(candidate_timestamp = self.restore_timestamp,
                                                                      master_datadir = self.master_datadir).run()
        ValidateRestoreTables(restore_tables = self.restore_tables,
                              restore_db = restore_db,
                              master_port = self.master_port).run()

        if self.ddboost is not None:
            logger.info("Copying dump %s from DDBoost..." % self.restore_timestamp)            
            RecoverDDBoostDumps(restore_timestamp = self.restore_timestamp,
                                master_port = self.master_port,
                                batch_default = self.batch_default,
                                user = self.ddboost['user'],
                                passwd = self.ddboost['passwd'],
                                host = self.ddboost['host']).run()

        fake_timestamp = BuildAllTableDumps(restore_timestamp = self.restore_timestamp,
                                            compress = compress,
                                            restore_tables = self.restore_tables,
                                            batch_default = self.batch_default,
                                            master_datadir = self.master_datadir,
                                            master_port = self.master_port).run()
        # Dump files reside on segments now, so there's no need for gp_dump to go out
        # to the DDBoost storage unit. Thus, ddboost = None here.
        RestoreDatabase(restore_timestamp = fake_timestamp,
                        no_analyze = True,
                        drop_db = False,
                        restore_global = False,
                        master_datadir = self.master_datadir,
                        master_port = self.master_port,
                        ddboost = None).run()                             
        if not self.keep_dump_files:
            ClearAllTableDumps(fake_timestamp = fake_timestamp,
                               compress = compress,
                               batch_default = self.batch_default,
                               master_datadir = self.master_datadir,
                               master_port = self.master_port).run()
        if not self.no_analyze:
            self._analyze(restore_db, self.restore_tables, self.master_port)
    def _analyze(self, restore_db, restore_tables, master_port):
        conn = None
        try:
            dburl = dbconn.DbURL(port=master_port, dbname=restore_db)
            conn = dbconn.connect(dburl)
            for table in restore_tables:
                logger.info('Commencing analyze of %s in %s database, please wait...' % (table, restore_db))
                try:
                    execSQL(conn, 'analyze %s' % table)
                except Exception, e:
                    logger.warn('Issue with analyze of %s table, check log file for details' % table)
                else:
                    logger.info('Analyze of %s table completed without error' % table)
        finally:
            if conn is not None:
                conn.close()



class ValidateRestoreTables(Operation):
    def __init__(self, restore_tables, restore_db, master_port): 
        self.restore_tables = restore_tables
        self.restore_db = restore_db
        self.master_port = master_port
    def execute(self): 
        existing_tables = []
        table_counts = []
        conn = None
        try:
            dburl = dbconn.DbURL(port=self.master_port, dbname=self.restore_db)
            conn = dbconn.connect(dburl)
            for restore_table in self.restore_tables:
                if '.' not in restore_table:
                    logger.warn("No schema name supplied for %s, removing from list of tables to restore" % restore_table)
                    continue

                schema, table = restore_table.split('.')
                count = execSQLForSingleton(conn, "select count(*) from pg_class, pg_namespace where pg_class.relname = '%s' and pg_class.relnamespace = pg_namespace.oid and pg_namespace.nspname = '%s'" % (table, schema))
                if count == 0:
                    logger.warn("Table %s does not exist in database %s, removing from list of tables to restore" % (table, self.restore_db))
                    continue

                count = execSQLForSingleton(conn, "select count(*) from %s.%s" % (schema, table))
                if count > 0:
                    logger.warn('Table %s has %d records %s' % (restore_table, count, WARN_MARK))
                existing_tables.append(restore_table)
                table_counts.append((restore_table, count))
        finally:
            if conn is not None:
                conn.close()

        if len(existing_tables) == 0:
            raise ExceptionNoStackTraceNeeded("Have no tables to restore")
        logger.info("Have %d tables to restore, will continue" % len(existing_tables))

        return (existing_tables, table_counts)



class BuildAllTableDumps(Operation):
    def __init__(self, restore_timestamp, compress, restore_tables, batch_default, master_datadir, master_port): 
        self.restore_timestamp = restore_timestamp
        self.compress = compress
        self.restore_tables = restore_tables
        self.batch_default = batch_default
        self.master_datadir = master_datadir
        self.master_port = master_port
    def execute(self): 
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)  
        # Create a fake timestamp for restore dump files
        for fake_time in range(0, 1000000):
            fake_timestamp = "%s%06d" % (self.restore_timestamp[0:8], fake_time)
            path = os.path.join(self.master_datadir, DUMP_DIR, fake_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, fake_timestamp))
            if self.compress:
                path += '.gz'
            if not CheckFile(path).run():
                break
        else:
            raise ExceptionNoStackTraceNeeded("Could not construct table dump")

        primaries = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]
        operations = []
        for seg in primaries:
            real_filename = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, self.restore_timestamp[0:8], "%s0_%d_%s" % (DBDUMP_PREFIX, seg.getSegmentDbId(), self.restore_timestamp))
            fake_filename = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, fake_timestamp[0:8], "%s0_%d_%s" % (DBDUMP_PREFIX, seg.getSegmentDbId(), fake_timestamp))
            operations.append( BuildRemoteTableDump(self.restore_tables, real_filename, fake_filename, self.compress, seg.getSegmentHostName()) )

        ParallelOperation(operations, self.batch_default).run()
        for operation in operations:
            try:
                operation.get_ret()
            except Exception, e:
                logger.exception('Parallel table dump file build failed.')
                raise ExceptionNoStackTraceNeeded('Parallel table dump file build failed, review log file for details')

        # Build master dump file
        real_filename = os.path.join(self.master_datadir, DUMP_DIR, self.restore_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, self.restore_timestamp))
        fake_filename = os.path.join(self.master_datadir, DUMP_DIR, fake_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, fake_timestamp))
        real_file, fake_file = None, None
        try:
            if self.compress:
                real_file = gzip.open(real_filename + '.gz', 'r')
                fake_file = gzip.open(fake_filename + '.gz', 'w')
            else:
                real_file = open(real_filename, 'r')
                fake_file = open(fake_filename, 'w')

            # TODO: copy over data among the first twenty lines that begin with 'SET'. Why?
            # gpdbrestore.sh : 1025
            output = False
            for line_no in range(0, 20):
                line = real_file.readline()
                output = line.startswith("SET")
                if output:
                    fake_file.write(line)
        except Exception, e:
            raise ExceptionNoStackTraceNeeded('Master dump file build failed, review log file for details')
        finally:
            if real_file is not None:
                real_file.close()
            if fake_file is not None:
                fake_file.close()

        # Build master cdatabase file
        real_createdb = os.path.join(self.master_datadir, DUMP_DIR, self.restore_timestamp[0:8], "%s%s" % (CREATEDB_PREFIX, self.restore_timestamp))
        fake_createdb = os.path.join(self.master_datadir, DUMP_DIR, fake_timestamp[0:8], "%s%s" % (CREATEDB_PREFIX, fake_timestamp))
        shutil.copy(real_createdb, fake_createdb)

        return fake_timestamp



class ClearAllTableDumps(Operation):
    """ TODO: This could be construed as the undo/rollback of BuildAllTable Dumps. """
    def __init__(self, fake_timestamp, compress, batch_default, master_datadir, master_port): 
        self.fake_timestamp = fake_timestamp
        self.compress = compress
        self.batch_default = batch_default
        self.master_datadir = master_datadir
        self.master_port = master_port
    def execute(self): 
        logger.info('Commencing deletion of temporary table dump files')

        # Remove master dump file
        path = os.path.join(self.master_datadir, DUMP_DIR, self.fake_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, self.fake_timestamp))
        if self.compress:
            path += '.gz'
        try:
            RemoveFile(path).run()
        except OSError, e:
            logger.warn('Failed to remove %s on master' % path)

        # Remove master cdatabase file
        path = os.path.join(self.master_datadir, DUMP_DIR, self.fake_timestamp[0:8], "%s%s" % (CREATEDB_PREFIX, self.fake_timestamp))
        try:
            RemoveFile(path).run()
        except OSError, e:
            logger.warn('Failed to remove %s on master' % path)

        # Remove segment dump files
        operations = []
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)
        primaries = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True)]
        for seg in primaries:
            path = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, self.fake_timestamp[0:8], "%s0_%d_%s" % (DBDUMP_PREFIX, seg.getSegmentDbId(), self.fake_timestamp))
            if self.compress:
                path += '.gz'
            host = seg.getSegmentHostName()
            operations.append(RemoveRemoteFile(path, host))

        ParallelOperation(operations, self.batch_default).run()
        for operation in operations:
            try:    
                operation.get_ret()
            except OSError, e:
                logger.warn('Failed to remove %s on %s' % (path, host))



def BuildRemoteTableDump(restore_tables, real_filename, fake_filename, compress, host):
    return RemoteOperation(BuildTableDump(restore_tables, real_filename, fake_filename, compress), host)



class BuildTableDump(Operation):
    def __init__(self, restore_tables, real_filename, fake_filename, compress):
        self.restore_tables = restore_tables
        self.real_filename = real_filename
        self.fake_filename = fake_filename
        self.compress = compress
    def execute(self):
        logger.info('Building dump file header')
        real_file, fake_file = None, None
        try:
            if self.compress:
                real_file = gzip.open(self.real_filename + '.gz', 'r')
                fake_file = gzip.open(self.fake_filename + '.gz', 'w')
            else:
                real_file = open(self.real_filename, 'r')
                fake_file = open(self.fake_filename, 'w')
             
            for restore_table in self.restore_tables:
                schema, table = restore_table.split('.')
                logger.info('Commencing data extract for %s' % table)
                real_file.seek(0, os.SEEK_SET)
                output, in_schema, found = False, False, False
                while True:
                    line = real_file.readline()
                    if not line:
                        break                

                    if line.startswith("SET search_path = %s," % schema):
                        fake_file.write(line)
                        schema = True
                    elif schema and line.startswith('COPY %s ' % table):
                        found = True
                        output = True
                    elif output and line.endswith('\\.'):
                        fake_file.write(line)
                        output = False
                        schema = False
                    if output and schema: 
                        fake_file.write(line)
        except Exception, e:
            # TODO: Don't catch all. Throw custom exception to parent Operation.
            logger.exception('Parallel table dump file build failed.')
            raise Exception('Parallel table dump file build failed, review log file for details')
        finally:
            if real_file is not None:
                real_file.close()
            if fake_file is not None:
                fake_file.close()



class GetDbName(Operation):
    def __init__(self, createdb_file):
        self.createdb_file = createdb_file
    def execute(self):
        f = open(self.createdb_file, 'r')
        # assumption: 'CREATE DATABASE' line will reside within the first 50 lines of the gp_cdatabase_1_1_* file
        for line_no in range(0, 50):
            line = f.readline()
            if not line:
                break
            if line.startswith("CREATE DATABASE"):
                restore_db = line.split()[2]
                return restore_db
        else:
            raise GetDbName.DbNameGiveUp()
        raise GetDbName.DbNameNotFound()

    class DbNameNotFound(Exception): pass
    class DbNameGiveUp(Exception): pass



class RecoverRemoteDumps(Operation):
    def __init__(self, host, path, restore_timestamp, compress, restore_global, batch_default, master_datadir, master_port):
        self.host = host
        self.path = path
        self.restore_timestamp = restore_timestamp
        self.compress = compress
        self.restore_global = restore_global
        self.batch_default = batch_default
        self.master_datadir = master_datadir
        self.master_port = master_port
    def execute(self): 
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)
        from_host, from_path = self.host, self.path
        logger.info("Commencing remote database dump file recovery process, please wait...")
        segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True) or seg.isSegmentMaster()]
        pool = WorkerPool(numWorkers = min(len(segs), self.batch_default))
        for seg in segs:
            if seg.isSegmentMaster():
                file = '%s%s' % (MASTER_DBDUMP_PREFIX, self.restore_timestamp)
            else:
                file = '%s0_%d_%s' % (DBDUMP_PREFIX, seg.getSegmentDbId(), self.restore_timestamp)
            if self.compress:
                file += '.gz'

            to_host = seg.getSegmentHostName()
            to_path = os.path.join(seg.getSegmentDataDirectory(), DUMP_DIR, self.restore_timestamp[0:8])
            if not CheckRemoteDir(to_path, to_host).run():
                logger.info('Creating directory %s on %s' % (to_path, to_host))
                try:
                    MakeRemoteDir(to_path, to_host).run()
                except OSError, e:
                    raise ExceptionNoStackTraceNeeded("Failed to create directory %s on %s" % (to_path, to_host))
   
            logger.info("Commencing remote copy from %s to %s:%s" % (from_host, to_host, to_path))
            pool.addCommand(Scp('Copying dump for seg %d' % seg.getSegmentDbId(),
                            srcFile=os.path.join(from_path, file),
                            dstFile=os.path.join(to_path, file),
                            srcHost=from_host,
                            dstHost=to_host))
        createdb_file = "%s%s" % (CREATEDB_PREFIX, self.restore_timestamp)
        to_path = os.path.join(self.master_datadir, DUMP_DIR, self.restore_timestamp[0:8])
        pool.addCommand(Scp('Copying schema dump',
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, createdb_file),
                            dstFile=os.path.join(to_path, createdb_file)))
        if self.restore_global:
            global_file = "%s%s" % (GLOBAL_PREFIX, self.restore_timestamp[0:8])
            pool.addCommand(Scp("Copying global dump",
                            srcHost=from_host,
                            srcFile=os.path.join(from_path, global_file),
                            dstFile=os.path.join(to_path, global_file)))
        pool.join()
        pool.check_results()



class RecoverDDBoostDumps(Operation):
    """
    This Operation is responsible for bringing all dumps for a particular timestamp back to the 
    segment hosts from the DDBoost storage unit. It is almost identical to RecoverRemoteDumps
    except that gpddboost is used to recover dumps, instead of scp.
    """
    def __init__(self, restore_timestamp, master_port, batch_default, user, passwd, host):
        self.restore_timestamp = restore_timestamp
        self.master_port = master_port
        self.batch_default = batch_default
        self.user = user
        self.passwd = passwd
        self.host = host
    def execute(self):
        logger.info("Commencing ddboost database dump file recovery process, please wait...")
        gparray = GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)
        segs = [seg for seg in gparray.getDbList() if seg.isSegmentPrimary(current_role=True) or seg.isSegmentMaster()]
        pool = WorkerPool(numWorkers = min(len(segs), self.batch_default))
        for seg in segs:
            if seg.isSegmentMaster():
                file = '%s%s' % (MASTER_DBDUMP_PREFIX, self.restore_timestamp)
            else:
                file = '%s0_%d_%s' % (DBDUMP_PREFIX, seg.getSegmentDbId(), self.restore_timestamp)

            to_host = seg.getSegmentHostName()
            from_path = os.path.join(DUMP_DIR, self.restore_timestamp[0:8])
            to_path = os.path.join(seg.getSegmentDataDirectory(), from_path)
            if not CheckRemoteDir(to_path, to_host).run():
                logger.info('Creating directory %s on %s' % (to_path, to_host))
                try:
                    MakeRemoteDir(to_path, to_host).run()
                except OSError, e:
                    raise ExceptionNoStackTraceNeeded("Failed to create directory %s on %s" % (to_path, to_host))

            from_path = os.path.join(from_path, file)
            to_path = os.path.join(to_path, file)

            logger.info("Commencing remote copy from DDBoost to %s:%s" % (to_host, to_path))
            pool.addCommand(Command('DDBoost copy for seg %d' % seg.getSegmentDbId(),
                                    'gpddboost --copyFromDDBoost --from-file=%s --to-file=%s --host=%s --user=%s --passwd=%s' % (from_path, to_path, self.host, self.user, self.passwd),
                                    remoteHost = to_host))
        pool.join()
        pool.check_results()



class GetDumpTables(Operation):
    def __init__(self, restore_timestamp, master_datadir, ddboost): 
        self.master_datadir = master_datadir
        self.restore_timestamp = restore_timestamp
        self.ddboost = ddboost
    def execute(self): 
        (restore_timestamp, restore_db, compress) = ValidateTimestamp(master_datadir = self.master_datadir,
                                                                      candidate_timestamp = self.restore_timestamp,
                                                                      ddboost = self.ddboost).run()
        dump_file = os.path.join(self.master_datadir, DUMP_DIR, restore_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, restore_timestamp))

        if self.ddboost is not None:
            from_file = os.path.join(DUMP_DIR, restore_timestamp[0:8], "%s%s" % (MASTER_DBDUMP_PREFIX, restore_timestamp))
            cmd = Command('DDBoost copy of master dump file',
                          'gpddboost --copyFromDDBoost --from-file=%s --to-file=%s --host=%s --user=%s --passwd=%s' 
                            % (from_file, dump_file, self.ddboost['host'], self.ddboost['user'], self.ddboost['passwd']))
            cmd.run(validateAfter = True)

        f = None
        schema = ''
        owner = ''
        ret = []
        try:
            if compress:
                f = gzip.open(dump_file + '.gz', 'r')
            else:
                f = open(dump_file, 'r')

            while True:
                line = f.readline()
                if not line:
                    break
                if line.startswith("SET search_path = "):
                    line = line[len("SET search_path = ") : ]
                    if ", pg_catalog;" in line:
                        schema = line[ : line.index(", pg_catalog;")]
                    else:
                        schema = "pg_catalog"
                elif line.startswith("-- Data for Name: "):
                    owner = line[line.index("; Owner: ") + 9 : ].rstrip()
                elif line.startswith("COPY "):
                    table = line[5:]
                    if table.rstrip().endswith(") FROM stdin;"):
                        if table.startswith("\""):
                            table = table[: table.index("\" (") + 1]
                        else:
                            table = table[: table.index(" (")]
                    else:
                        table = table[: table.index(" FROM stdin;")]
                    table = table.rstrip()
                    ret.append( (schema, table, owner) )
        finally:
            if f is not None:
                f.close()
        return ret
