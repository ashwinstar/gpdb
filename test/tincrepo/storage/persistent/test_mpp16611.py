import logging
from multiprocessing import Process, JoinableQueue
import os
import subprocess
import sys
import tempfile

import pexpect
import unittest2 as unittest
from tinctest.models.gpdb import GPDBTestCase
from tinctest.lib import Gpdiff, PSQL, local_path
import tinctest.lib.gpplatform as gpplatform


logger = logging.getLogger(__name__)
logger.addHandler(logging.StreamHandler(sys.stdout))
logger.setLevel(logging.DEBUG)

def get_last_backend_pid():
    cmd = (r'psql -A -t -c '
            '"select procpid from pg_stat_activity '
            'where procpid <> pg_backend_pid() '
            'order by procpid desc limit 1"')

    output = subprocess.Popen(cmd, shell=True,
                              stdout=subprocess.PIPE).communicate()[0]
    return int(output)

@unittest.skipIf(gpplatform.get_info()=="SOL", "gdb is not installed on Solaris regression server")
class mpp16611(GPDBTestCase):

    def test_storage_persistent(self):
        """
        @author haradh1
        @description Test for the RelcacheInvalidate VS persistent table
        @created 2013-02-12 00:00:00
        @modified 2013-02-12 00:00:00
        @tags mpp16160

        The issue of mpp16611 is that persistent table (PT) information
        in Relation cache is lost during heap_insert after fetching it
        from gp_relation_node, if concurrent cache invalidation message
        overflows, and if the heap_insert goes saving values to toast
        table.  We should restore the PT info in that case.

        The following is the series of events.

            - Session 1 inserts a row worthy of toasting into a table.
            - After session 1 picks up the gp_relation_node information for
              this table, but prior to relation_open, Session 2 jumps in.
            - Session 2 needs to overflow shared invalidation message, which
              causes Session 1 to rebuild all relcache (and catcache) rebuild
              regardless whether the cache entries actually need to be
              updated. The overflow threshold is defined by MAXNUMMESSAGES
              in sinvaladt.h
            - Session 1 proceeds to relation_open and
              AcceptInvalidationMessages, and during RelationClearRelation,
              it rebuilds its relcache information which erroneously loses
              the gp_relation_node information.
            - Session 1 persists this incorrect gp_relation_node information
              to the xlog during XLogInsert.
            - If we then crash immediately, and rely upon xlog replay during
              crash recovery, we hit the heap_insert_redo error.  It seems
              we hit the heap_insert_redo error in segments, but in master it
              just loses the data, which this test shows.
        """

        psql_prompt = r'\w+\=\# '
        gdb_prompt = r'\(gdb\)'

        # open psql first.  don't use readline, as it's gonna have
        # problem under tty scripting...
        psql1 = pexpect.spawn('psql -n')
        psql1.expect(psql_prompt)

        # attach to the QD process
        pid = get_last_backend_pid()
        gdb = pexpect.spawn('gdb -p %d' % pid)
        gdb.expect(gdb_prompt)

        # set breakpoint, with relation == 2609 == pg_description.
        # pg_class/pg_proc etc are not usable here as they are nailed
        # relation which don't rebuild at the invalidation time.
        #
        # This should come during heap_insert, right after
        # RelationFetchGpRelationNodeForXLog, where we fetched
        # the persistent table info into Relation.
        gdb.sendline('b toast_insert_or_update if rel->rd_id == 2609')
        gdb.expect(gdb_prompt)
        logger.debug(gdb.before)

        gdb.sendline('c')
        # Don't expect prompt until the breakpoint hit.

        # create a long comment so that heap_insert goes into
        # tuple_insert_or_update, which leads to relation_open
        # and AcceptInvalidationMessages
        # Note we'd better avoid multiple inputs which cause
        # a lot of prompt string in the terminal.
        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""
            drop function if exists f();
            create or replace function f() returns void as $$
            declare begin return; end;
            $$ language plpgsql;
            comment on function f() is $$
        """)
        sql_script.write('comment_to_force_toast,\n' * 1000000)
        sql_script.write('$$;')
        sql_script.flush()
        psql1.sendline(r'\i ' + sql_script.name)
        # Don't expect prompt until it's ready.

        # Now, GDB should hit the breakpoint.
        gdb.expect(gdb_prompt)
        logger.debug(gdb.before)

        # open the next psql in order to cause concurrent
        # invalidation message overflow.
        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""set client_min_messages = warning;
          drop table if exists part;
          create table part(a int, b int)
            partition by range(b)
            (start (1) end(500) every(1));
          alter table part add column c int default 1;
          drop table part;""")
        sql_script.flush()
        psql2 = pexpect.spawn('psql -n')
        psql2.expect(psql_prompt)
        psql2.sendline(r'\i ' + sql_script.name)
        psql2.expect(psql_prompt)
        logger.debug(psql2.before)
        psql2.sendline(r'\q')
        psql2.close()

        # hit q to finish the rest of work in psql1.
        gdb.sendline('q')
        gdb.close()

        # make sure everything is done.
        psql1.expect(psql_prompt)
        logger.debug(psql1.before)
        psql1.sendline(r'\q')
        psql1.close()

        # determine the results.
        sql_file = local_path('mpp16611.sql')
        ans_file = local_path('mpp16611.ans')

        out_file = local_path('mpp16611.1.out')
        PSQL.run_sql_file(sql_file=sql_file,
                          out_file=out_file, output_to_file=True)
        assert Gpdiff.are_files_equal(out_file, ans_file)

        # cause recovery.  If persistent table information is
        # not correctly stored at XLogInsert, we'll fail.
        # There still could be possibilities that a checkpoint
        # happens by checkpoint process, in which case this test
        # doesn't show the bug, but it's quite unlikely.
        subprocess.check_call(r'source $GPHOME/greenplum_path.sh; gpstop -air', shell=True)

        out_file = local_path('mpp16611.2.out')
        PSQL.run_sql_file(sql_file=sql_file,
                          out_file=out_file, output_to_file=True)

        assert Gpdiff.are_files_equal(out_file, ans_file)

if __name__ == '__main__':
    main()
