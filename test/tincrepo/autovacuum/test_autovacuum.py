from os.path import abspath, dirname
import sys
import unittest2 as unittest
from tinctest import TINCTestCase, logger
from tinctest.lib import Gpdiff, local_path, PSQL
from tinctest.models.gpdb.sql.template import SQLTemplateTestCase
import tinctest.lib.gpplatform as gpplatform
from gppylib.commands.base import Command
from gppylib.db import dbconn
from gppylib.gparray import GpArray

FirstNormalTransactionId = 3
def xid_sum(x, y):
    """
    XID arithmetic is tricky. See GetNewTransactionId for more information.
    """
    ret = (x + y) % (2 ** 32)
    if ret <= FirstNormalTransactionId:
        ret += FirstNormalTransactionId
    return ret

@unittest.skipIf(gpplatform.get_info()=="SOL", "Skipping Solaris, cannot compile xidhelper.so")
class AutovacuumTests(TINCTestCase):
    def setUp(self):
        Command('re-build regress.so', 
                'make -C %s xidhelper.so' % local_path('.')).run(validateAfter=True)
        SQLTemplateTestCase.perform_transformation_on_sqlfile(local_path('load_xidhelper.sql'),
                                                              local_path('load_xidhelper.sql.t'),
                                                              {'@source@' : local_path('xidhelper.so')})
        PSQL.run_sql_file(sql_file=local_path('load_xidhelper.sql.t'), 
                          out_file=local_path('load_xidhelper.out.t'))

    def _basic_sanity_check(self, suffix):
        """
        Runs sanity.sql which contains a CREATE, INSERT, SELECT, and DROP.
        """
        sql_file = local_path('sanity_%s.sql' % suffix)
        out_file = local_path('sanity_%s.out' % suffix)
        ans_file = local_path('sanity_%s.ans' % suffix)
        try:
            PSQL.run_sql_file(sql_file=sql_file,
                              out_file=out_file, 
                              output_to_file=True)
        except:
            pass
        assert Gpdiff.are_files_equal(out_file, 
                                      ans_file, 
                                      match_sub=[local_path('init_file')])

    def _get_primary_mirror_pair(self):
        primary, mirror = None, None
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        for seg in gparray.getSegDbList(True):
            if seg.content == 0:
                if seg.role == 'p':
                    primary = seg
                else:
                    mirror = seg
        return (primary, mirror)

    def test_autovacuum_signaling(self):
        """
        Raise the nextXid to oldest_frozenxid + autovacuum_freeze_max_age.
        Run a transaction.
        Ensure that no autovacuum daemon is started.
        """
        dburl = dbconn.DbURL()
        with dbconn.connect(dburl) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return master to xid %d' % teardown_next_xid)
            def cleanup():
                with dbconn.connect(dburl) as myconn:
                    dbconn.execSQLForSingleton(myconn, "select spoof_next_xid('%d'::xid)" % teardown_next_xid)
                    dbconn.execSQL(myconn, "checkpoint")
                self._basic_sanity_check('clean')
                logger.info('Returned master to xid %d' % teardown_next_xid)
            self.addCleanup(cleanup)

            oldest_xid = int(dbconn.execSQLForSingleton(conn, 'select get_oldest_xid()'))
            autovacuum_freeze_max_age = int(dbconn.execSQLForSingleton(conn, 'show autovacuum_freeze_max_age'))
            autovacuum_xid_limit = xid_sum(oldest_xid, autovacuum_freeze_max_age)
            logger.info('Raising master xid to autovacuum_xid_limit %d' % autovacuum_xid_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % autovacuum_xid_limit)

        # A new connection to the postmaster, at this point, will ensure that we roll through
        # the ServerLoop and potentially fork an autovacuum process... if enabled.
        # Burn a transaction to trigger any undesirable behavior that we're disabling.
        with dbconn.connect(dburl) as conn:
            self.assertEqual(1, int(dbconn.execSQLForSingleton(conn, 'select 1')))

        cmd = Command('check for autovacuum',
                      'ps -ef | grep -v grep | grep postgres | grep autovacuum')
        cmd.run()
        self.assertEqual(cmd.get_results().stdout, "", "Seriously? Found a postgres autovacuum process!")

        self._basic_sanity_check('clean')

    def test_autovacuum_signaling_on_segment(self):
        """
        Same as above, but on a segment.
        """
        # connect to the master to build gparray
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        primary, _ = self._get_primary_mirror_pair()
        logger.info('Isolated segment %d at %s:%d' % (primary.dbid, primary.hostname, primary.port))
        dburl = dbconn.DbURL(hostname=primary.hostname, port=primary.port)

        with dbconn.connect(dburl, utility=True) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return segment to xid %d' % teardown_next_xid)
            def cleanup():
                with dbconn.connect(dburl, utility=True) as myconn:
                    dbconn.execSQLForSingleton(myconn, "select spoof_next_xid('%d'::xid) "% teardown_next_xid)
                    dbconn.execSQL(myconn, "checkpoint")
                self._basic_sanity_check('clean')
                logger.info('Returned segment to xid %d' % teardown_next_xid)
            self.addCleanup(cleanup)

            oldest_xid = int(dbconn.execSQLForSingleton(conn, 'select get_oldest_xid()'))
            autovacuum_freeze_max_age = int(dbconn.execSQLForSingleton(conn, 'show autovacuum_freeze_max_age'))
            autovacuum_xid_limit = xid_sum(oldest_xid, autovacuum_freeze_max_age)
            logger.info('Raising segment xid to autovacuum_xid_limit %d' % autovacuum_xid_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % autovacuum_xid_limit)

        # A new connection to the postmaster, at this point, will ensure that we roll through
        # the ServerLoop and potentially fork an autovacuum process... if enabled.
        with dbconn.connect(dburl, utility=True) as conn:
            self.assertEqual(1, int(dbconn.execSQLForSingleton(conn, 'select 1')))

        cmd = Command('check for autovacuum',
                      'ssh %s ps -ef | grep -v grep | grep postgres | grep autovacuum' % primary.hostname)
        cmd.run()
        self.assertEqual(cmd.get_results().stdout, "", "Seriously? Found a postgres autovacuum process!")

        self._basic_sanity_check('clean')

    def test_warn_limit(self):
        """
        Raise the nextXid to the warning limit.
        """
        dburl = dbconn.DbURL()
        with dbconn.connect(dburl) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return master to xid %d' % teardown_next_xid)
            def cleanup():
                PSQL.run_sql_command(sql_cmd="select spoof_next_xid('%d'::xid)" % teardown_next_xid)
                PSQL.run_sql_command(sql_cmd="checkpoint")
                self._basic_sanity_check('clean')
                logger.info('Returned master to xid %d' % teardown_next_xid)
            self.addCleanup(cleanup)

            warn_limit = int(dbconn.execSQLForSingleton(conn, 'select get_warn_limit()'))
            logger.info('Raising master xid to warn limit %d' % warn_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % warn_limit)

        self._basic_sanity_check('warn')

    def test_warn_limit_on_segment(self):
        """
        Same as above, but on a segment.
        """
        # connect to the master to build gparray
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        primary, _ = self._get_primary_mirror_pair()
        logger.info('Isolated segment %d at %s:%d' % (primary.dbid, primary.hostname, primary.port))
        dburl = dbconn.DbURL(hostname=primary.hostname, port=primary.port)

        with dbconn.connect(dburl, utility=True) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return segment to xid %d' % teardown_next_xid)
            def cleanup():
                PSQL.run_sql_command(host=primary.hostname, 
                                     port=primary.port, 
                                     PGOPTIONS="-c 'gp_session_role=utility'",
                                     sql_cmd="select spoof_next_xid('%d'::xid)" % teardown_next_xid)
                PSQL.run_sql_command(host=primary.hostname, 
                                     port=primary.port, 
                                     PGOPTIONS="-c 'gp_session_role=utility'",
                                     sql_cmd="checkpoint")
                self._basic_sanity_check('clean')
                logger.info('Returned segment to xid %d' % teardown_next_xid)
            self.addCleanup(cleanup)

            warn_limit = int(dbconn.execSQLForSingleton(conn, 'select get_warn_limit()'))
            logger.info('Raising segment xid to warn limit %d' % warn_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % warn_limit)

        self._basic_sanity_check('warn_segment')

    def test_stop_limit(self):
        """
        Raise the nextXid to the stop limit.
        Ensure system errors out.

        Note: in the case of the stop limit, we cannot revert using spoof_next_xid again as this
        requires allocating a xid. In this scenario, we have to bring down the node and pg_resetxlog
        its way back home.
        """
        dburl = dbconn.DbURL()
        gparray = GpArray.initFromCatalog(dburl, utility=True)
        with dbconn.connect(dburl) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return master to xid %d' % teardown_next_xid)
            def cleanup():
                for seg in gparray.getDbList(True):
                    logger.info('Stopping segment %d at %s' % (seg.dbid, seg.datadir))
                    Command('stop segment', 'pg_ctl -D %s stop' % seg.datadir).run(validateAfter=True)
                logger.info('Resetting xlog on master to xid %d' % teardown_next_xid)
                Command('reset xlog', 'pg_resetxlog -x %d -f $MASTER_DATA_DIRECTORY' % teardown_next_xid).run(validateAfter=True)
                logger.info('Starting the cluster')
                Command('start system', 'source $GPHOME/greenplum_path.sh && gpstart -a').run(validateAfter=True)
                self._basic_sanity_check('clean')
                logger.info('Returned master to xid %d' % teardown_next_xid)
            self.addCleanup(cleanup)

            stop_limit = int(dbconn.execSQLForSingleton(conn, 'select get_stop_limit()'))
            logger.info('Raising master xid to stop limit %d' % stop_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % stop_limit)

        self._basic_sanity_check('error')

    def test_stop_limit_on_segment(self):
        """
        Same as above, but on a segment.
        """
        # connect to the master to build gparray
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        primary, mirror = self._get_primary_mirror_pair()
        logger.info('Isolated primary %d at %s:%d' % (primary.dbid, primary.hostname, primary.port))
        dburl = dbconn.DbURL(hostname=primary.hostname, port=primary.port)

        with dbconn.connect(dburl, utility=True) as conn:
            teardown_next_xid = int(dbconn.execSQLForSingleton(conn, 'select get_next_xid()'))
            teardown_next_xid = xid_sum(teardown_next_xid, 1)         # we burned a xid when trying to ascertain nextXid
            logger.info('Intending to return segment to xid %d' % teardown_next_xid)
            def cleanup():
                logger.info('Stopping the cluster')
                Command('stop system', 'source $GPHOME/greenplum_path.sh && gpstop -a').run(validateAfter=True)
                logger.info('Resetting xlog on primary to xid %d' % teardown_next_xid)
                Command('reset xlog', 'ssh %s source $GPHOME/greenplum_path.sh && pg_resetxlog -x %d -f %s' % (primary.hostname, teardown_next_xid, primary.datadir)).run(validateAfter=True)
                if mirror is not None:
                    logger.info('Resetting xlog on mirror to xid %d' % teardown_next_xid)
                    Command('reset xlog', 'ssh %s source $GPHOME/greenplum_path.sh && pg_resetxlog -x %d -f %s' % (mirror.hostname, teardown_next_xid, mirror.datadir)).run(validateAfter=True)
                logger.info('Starting the cluster')
                Command('start system', 'source $GPHOME/greenplum_path.sh && gpstart -a').run(validateAfter=True)
                self._basic_sanity_check('clean')
                logger.info('Returned segment to xid %d' % teardown_next_xid)

            self.addCleanup(cleanup)

            stop_limit = int(dbconn.execSQLForSingleton(conn, 'select get_stop_limit()'))
            logger.info('Raising segment xid to stop limit %d' % stop_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % stop_limit)

        self._basic_sanity_check('error_segment')

    def test_vacuum_recovery_at_warn_limit(self):
        """
        Raise the master to the warn limit.
        VACUUM FREEZE all databases.
        Run the sanity pass.
        There's no explicit cleanup, since the test itself is cleanup.
        """
        dburl = dbconn.DbURL()
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        databases = []
        with dbconn.connect(dburl, utility=True) as conn:
            for row in dbconn.execSQL(conn, "select datname from pg_database"):
                databases.append(row[0])

        # helper function to open/close access to template0
        def set_allowconn_template0(flag):
            logger.info("opening up template0")
            for seg in gparray.getDbList(True):
                if seg.role == 'p':
                    seg_url = dbconn.DbURL(hostname=seg.hostname, port=seg.port)
                    with dbconn.connect(seg_url, utility=True, allowSystemTableMods='dml') as conn:
                        dbconn.execSQL(conn, "update pg_database set datallowconn=%s where datname='template0'" % flag)
                        conn.commit()

        self.addCleanup(set_allowconn_template0, False)     # close template0 on exit
        set_allowconn_template0(True)                       # open template0

        with dbconn.connect(dburl, utility=True) as conn:
            warn_limit = int(dbconn.execSQLForSingleton(conn, 'select get_warn_limit()'))
            logger.info('Raising master xid to warn limit %d' % warn_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % warn_limit)

        for datname in databases:
            logger.info('vacuum freeze %s' % datname)
            PSQL(sql_cmd='VACUUM FREEZE',
                 dbname=datname,
                 out_file='vacuum_%s.out' % datname,
                 output_to_file=True).run(validateAfter=True)

        self._basic_sanity_check('clean')

    def test_vacuum_recovery_at_stop_limit(self):
        """
        Raise the master to the stop limit.
        pg_ctl the system down.
        Revise the stop limit GUC for each segment.
        Restart the cluster.
        VACUUM FREEZE all databases.
        Run the sanity pass.
        Undo the stop limit GUC change.
        """
        dburl = dbconn.DbURL()
        gparray = GpArray.initFromCatalog(dbconn.DbURL(), utility=True)
        databases = []
        with dbconn.connect(dburl, utility=True) as conn:
            xid_stop_limit = int(dbconn.execSQLForSingleton(conn, 'show xid_stop_limit'))
            new_stop_limit = xid_sum(xid_stop_limit, -(10**6))
            for row in dbconn.execSQL(conn, "select datname from pg_database"):
                databases.append(row[0])

        # Undo the stop limit changes
        def cleanup():
            logger.info('Undo the stop limit change')
            Command('stop system', 'source $GPHOME/greenplum_path.sh && gpstop -a').run(validateAfter=True)
            Command('undo xid_stop_limit change',
                    'sed -i".bk" "s|xid_stop_limit=%s||g" $MASTER_DATA_DIRECTORY/postgresql.conf' % new_stop_limit).run(validateAfter=True)
            Command('start system', 'source $GPHOME/greenplum_path.sh && gpstart -a').run(validateAfter=True)
        self.addCleanup(cleanup)            # revert postgresql.conf xid_stop_limit changes

        # helper function to open/close access to template0
        def set_allowconn_template0(flag):
            logger.info("opening up template0")
            for seg in gparray.getDbList(True):
                if seg.role == 'p':
                    seg_url = dbconn.DbURL(hostname=seg.hostname, port=seg.port)
                    with dbconn.connect(seg_url, utility=True, allowSystemTableMods='dml') as conn:
                        dbconn.execSQL(conn, "update pg_database set datallowconn=%s where datname='template0'" % flag)
                        conn.commit()

        self.addCleanup(set_allowconn_template0, False)     # close template0 on exit
        set_allowconn_template0(True)                       # open template0

        with dbconn.connect(dburl, utility=True) as conn:
            stop_limit = int(dbconn.execSQLForSingleton(conn, 'select get_stop_limit()'))
            logger.info('Raising master xid to stop limit %d' % stop_limit)
            dbconn.execSQLForSingleton(conn, "select spoof_next_xid('%d'::xid)" % stop_limit)

        self._basic_sanity_check('error')

        for seg in gparray.getDbList(True):
           logger.info('Stopping segment %d at %s' % (seg.dbid, seg.datadir))
           Command('stop segment', 'pg_ctl -D %s stop' % seg.datadir).run(validateAfter=True)

        # revise the stop limit on the master
        logger.info('revising xid_stop_limit on the master xid_stop_limit %s new_stop_limit %s' % (xid_stop_limit, new_stop_limit))
        Command('revise xid_stop_limit', 'echo "xid_stop_limit=%s" >> $MASTER_DATA_DIRECTORY/postgresql.conf' % new_stop_limit).run(validateAfter=True)
        # start the cluster
        logger.info('Starting the cluster')
        Command('start cluster', 'source $GPHOME/greenplum_path.sh && gpstart -a').run(validateAfter=True)

        # VACUUM FREEZE all databases
        for datname in databases:
            logger.info('vacuum freeze %s' % datname)
            PSQL(sql_cmd='VACUUM FREEZE',
                 dbname=datname,
                 out_file='vacuum_%s.out' % datname,
                 output_to_file=True).run(validateAfter=True)

        self._basic_sanity_check('clean')
