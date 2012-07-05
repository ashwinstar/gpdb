
import os, sys, logging, stat, errno
from time import localtime, strftime

try:
    from gppylib.gparray import GpArray
    from gppylib.gpversion import GpVersion
    from gppylib.db import dbconn
except ImportError, e:
    sys.exit('Error: unable to import module: ' + str(e))


MASTERDB = -2
SEGDBS   = -3

#-------------------------------------------------------------------------------
# Setup LOGGER
#-------------------------------------------------------------------------------
logger = logging.getLogger('gpcheckcat')
logger.setLevel(logging.INFO)

formatstr = '%(asctime)s:gpcheckcat:-[%(levelname)-s]:-%(message)s'
formatter = logging.Formatter(formatstr, '%Y%m%d:%H:%M:%S')
empty_formatter = logging.Formatter()

logdir = os.path.join(os.path.expanduser('~'), 'gpAdminLogs')
if not os.path.exists(logdir):
    os.mkdir(logdir)
logfile = '%s/gpcheckcat_%s.log' % (logdir, strftime('%Y%m%d_%H%M%S', localtime()))
fhandler = logging.FileHandler(logfile, 'w')
fhandler.setLevel(logging.DEBUG)
fhandler.setFormatter(formatter)
logger.addHandler(fhandler)

shandler = logging.StreamHandler()
shandler.setLevel(logging.CRITICAL)
shandler.setFormatter(empty_formatter)
logger.addHandler(shandler)

def log_literal(lvl, msg):
    """Log to the log file but without extra formatting."""
    fhandler.setFormatter(empty_formatter)
    logger.log(lvl, msg)
    fhandler.setFormatter(formatter)

def log_verbose():
    logger.setLevel(logging.DEBUG)

#-------------------------------------------------------------------------------
# Get GPDB version
#-------------------------------------------------------------------------------
def get_version():
    conn = dbconn.connect(dbconn.DbURL())
    version_str = dbconn.execSQLForSingleton(conn, "SELECT version()")
    version = GpVersion(version_str).getVersionRelease()
    logger.debug('gpdb version %s' % version)
    return version

DBVERSION = get_version()


#-------------------------------------------------------------------------------
# GPDB cluster configuration
#-------------------------------------------------------------------------------
class DBConfig:
    def __init__(self):
        self.gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        self.segment_map = {} # {segid : (host: host, port: port)}
        self._get_hosts_and_ports()

    def _get_hosts_and_ports(self):
        msegid = self.gparray.master.content
        mhost  = self.gparray.master.hostname
        mport  = self.gparray.master.port
        self.segment_map[msegid] = {'host': mhost, 'port': mport}
        for seg in self.gparray.segments:
            ssegid = seg.primaryDB.content
            shost  = seg.primaryDB.hostname
            sport  = seg.primaryDB.port
            self.segment_map[ssegid] = {'host': shost, 'port': sport}

DBCONFIG = DBConfig()


#-------------------------------------------------------------------------------
# gpcheckcat repair script
# exposed API: 
#        RSCRIPT.create_repair_script(dbname, segid, statement, filename, comment)
#        RSCRIPT.create_script_driver()
#-------------------------------------------------------------------------------
class RepairScriptDriver:
    """
    One RepairScriptDriver per gpcheckcat run 
        repair_script = {(dbname, segid, filename) : RepairScript object}
        driver_content = statements to execute all repair scripts
    """
    def __init__(self):
        self.repair_script  = {} 
        self.driver_content = '#!/bin/bash\ncd $(dirname $0)\n'  

    def create_repair_script(self, dbname, segid, filename, statement, comment = None):
        """User API to add repair scripts"""
        logger.info('Create repair scripts with filename = %s' % (filename))
        repair = self.repair_script.get((dbname, segid, filename), None)
        if repair is None:
            repair = RepairScript(dbname, segid, filename, comment)
            self.repair_script[(dbname, segid, filename)] = repair
        repair.add_statement(statement)

    def create_script_driver(self, dir):
        """User API to create script driver"""
        if not self.repair_script:
            return False

        logger.info('Create repair scripts and add statements to driver_content')
        self._create_script_dir(dir)
        for repair in self.repair_script.values():
            repair.write_to_file(dir)

        logger.info('Create the repair script driver')
        fullpath = '%s/runscript.sh' % (dir)
        with open(fullpath, 'w') as file:
            os.chmod(fullpath, 0700)
            file.write(self.driver_content)
        file.close()
        return True

    def _create_script_dir(self, dir):
        try:
            os.mkdir(dir)
        except OSError, e:
            if e.errno != errno.EEXIST:
                raise

RSCRIPT = RepairScriptDriver()

#-------------------------------------------------------------------------------
class RepairScript:

    def __init__(self, dbname, segid, filename, comment = None):
        self.dbname     = dbname
        self.segid      = segid
        self.filename   = filename + '.' + self.dbname
        self.statements = []
        self.comment    = ''
        if comment is not None: 
            # comment in the repair script driver
            self.comment = 'echo "%s"' % comment

    def add_statement(self, statement):
        self.statements.append(statement)

    def write_to_file(self, dir):
        """
        Create one file for each RepairScript object.
        Add execution statement to script driver file.
        """

        # formulate repair statements and output them to one or more files
        str  = 'BEGIN;\n' + '\n'.join(self.statements) + '\nCOMMIT;\n'
        mode = 'PGOPTIONS="-c gp_session_role=utility -c allow_system_table_mods=dml"'
        if DBVERSION < '4.2':
            mode = 'PGOPTIONS="-c gp_session_role=utility"'

        if MASTERDB == self.segid:
            logger.info('Create repair script run on the entry DB')
            fullpath = '%s/%s.sql' % (dir, self.filename)
            with open(fullpath, 'w') as file:
                file.write(str)
            RSCRIPT.driver_content += '\n%s\n' % self.comment 
            RSCRIPT.driver_content += 'psql -X -a -h %s -p %s -f %s "%s"> %s 2>&1 \n' \
                                      % (DBCONFIG.segment_map[-1]['host'],
                                         DBCONFIG.segment_map[-1]['port'],
                                         self.filename + '.sql',
                                         self.dbname,
                                         self.filename + '.sql.out')


        elif SEGDBS == self.segid:
            for id in DBCONFIG.segment_map:
                logger.info('Create repair script run on all segments')
                filename = '%s.seg%s.sql' % (self.filename, id)
                fullpath = '%s/%s' % (dir, filename)
                with open(fullpath, 'w') as file:
                    file.write(str)
                RSCRIPT.driver_content += '\n%s\n' % self.comment 
                RSCRIPT.driver_content += 'env %s psql -X -a -h %s -p %s -f %s "%s"> %s 2>&1 \n' \
                                          % (mode,
                                             DBCONFIG.segment_map[id]['host'],
                                             DBCONFIG.segment_map[id]['port'],
                                             filename,
                                             self.dbname,
                                             filename + '.out')

        else:
            logger.info('Create repair script run on particular segments')
            filename = '%s.seg%s.sql' % (self.filename, self.segid)
            fullpath = '%s/%s' % (dir, filename)
            with open(fullpath, 'w') as file:
                file.write(str)
            RSCRIPT.driver_content += '\n%s\n' % self.comment
            RSCRIPT.driver_content += 'env %s psql -X -a -h %s -p %s -f %s "%s"> %s 2>&1 \n' \
                                      % (mode,
                                         DBCONFIG.segment_map[self.segid]['host'],
                                         DBCONFIG.segment_map[self.segid]['port'],
                                         filename,
                                         self.dbname,
                                         filename + '.out')

    def __cmp__(self, other):
        assert(isinstance(other, RepairScript))
        return cmp((self.dbname, self.segid, self.filename), (other.dbname, other.segid, other.filename))


    def __hash__(self):
        return hash((self.dbname, self.segid, self.filename))


#===============================================================================
if __name__ == '__main__':

    # Unittest
    print 'segment DB: %s ' % (DBCONFIG.segment_map)

    # create one repair script to execute on segment 1
    RSCRIPT.create_repair_script('checkcatdb', 1, 'removeConstraint', 'ALTER TABLE ttable DROP CONSTRAINT;', 'Drop constraints')
    RSCRIPT.create_repair_script('checkcatdb', 1, 'removeConstraint', 'ALTER TABLE ttable2 DROP CONSTRAINT;', 'Drop constraints')
    RSCRIPT.create_repair_script('checkcatdb', 1, 'removeConstraint', 'ALTER TABLE ttable3 DROP CONSTRAINT;', 'Drop constraints')

    # create one repair script to run all the masterdb
    RSCRIPT.create_repair_script('gptest', MASTERDB, 'fixDistribution', 'ALTER TABLE parttab SET DISTRIBUTED RANDOMLY;', 'fix distribution policy') 

    # create one repair script per segment 
    statement  = '\n--Constraint Name: %s on %s becomes %s\n' % ('oldconname', 'relname', 'newconname') 
    statement += 'UPDATE pg_constraint\n'
    statement += 'SET conname = %s\n' % 'newconname'
    statement += 'WHERE conrelid = %d and conname = %s;\n' % (399832, 'oldconname')
    RSCRIPT.create_repair_script('gptest', SEGDBS, 'fixConstraintName', statement, 'fix partitioned table constraint name')

    # No comment
    RSCRIPT.create_repair_script('gptest', MASTERDB, 'fixSomething', 'ALTER TABLE parttab SET DISTRIBUTED RANDOMLY;') 

    # Create repair script driver
    RSCRIPT.create_script_driver('test_repair')

    
