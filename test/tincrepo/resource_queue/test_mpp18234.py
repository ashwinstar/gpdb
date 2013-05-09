import logging
from multiprocessing import Process, JoinableQueue
import os
import subprocess
import sys
import tempfile

import pexpect
import unittest2 as unittest
import tinctest.lib.gpplatform as gpplatform
from tinctest.models.gpdb import GPDBTestCase
from tinctest.lib import Gpdiff, PSQL, local_path

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
class mpp18234(GPDBTestCase):

    def test_resource_queue(self):
        """
        @author reddyr3
        @description Test for the proclock table corruption
        @created 2013-02-26 00:00:00
        @modified 2013-02-26 00:00:00
        @tags mpp18234

        The main issue is, if a session that is waiting for resource queue
        lock is terminated, the awaitedLock information is not cleared out. If the session had persistent
        table entries, abort processing acquires another lock, which reuses the same hash table slot
        as the "awaited" resource queue lock. awaitedLock is pointing to the wrong lock now, and this causes problems.

        The following is the series of events.

            - User u is associated with resource queue with 1 active slot.
            - Session 1 (user u) begins a transaction and creates a table (forces entries in persistent table).
            - Session 2 (user u) acquires a resource queue slot/lock.
            - Session 1 runs a query, and since there is no available resource queue slot, waits.
            - Sesion 1 gets canceled.
            - During cleanup, locallock associated with the resource queue lock
              is cleaned up from the hashtable. But the awaitedLock pointer and
              MyProc waitLock pointer are not cleaned out.
            - During AbortTransaction, another lock is acquired (LockRelationForResynchronize) during persistent
              table object cleanup, which reuses the cleaned up hash table slot, and this causes awaitedLock to
              point to the newly acquired lock.
            - Eventually, in AbortTransaction, we call ProcReleaseLocks->LockWaitCancel for the awaitedLock.
            - LockWaitCancel tries to clean out the awaitedLock, and it can no longer find it in the proclock table, due
              to the above sequence of events. i.e the hashcode used to lookup the entry in proclock table is
              computed from the resource queue lock + Resynchronize lock, which is wrong. We don't find the
              entry in the proclock table and get "proclock corrupted".
        """

        # default psql prompt
        psql_prompt = r'\w+\=\# '
        # psql prompt when setting role u
        psqlu_prompt = r'\w+\=\> '
        gdb_prompt = r'\(gdb\)'

        # open psql first.  don't use readline, as it's gonna have
        # problem under tty scripting...
        psql1 = pexpect.spawn('psql -n')
        psql1.expect(psql_prompt)
        pid1 = get_last_backend_pid()

        # session 1 - creates a table to force persistent table entry.
        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""set client_min_messages = warning;
            create resource queue q with (active_statements = 1);
            create role u with resource queue q;
            set role u;
            begin;
            create table foo as select 1;""")
        sql_script.flush()
        psql1.sendline(r'\i ' + sql_script.name)
        # Don't expect prompt until it's ready.

        psql2 = pexpect.spawn('psql -n')
        psql2.expect(psql_prompt)
        pid2 = get_last_backend_pid()

        # attach to the QD process
        gdb = pexpect.spawn('gdb -p %d' % pid2)
        gdb.expect(gdb_prompt)

        # set breakpoint, so session 2 acquires a resource queue lock
        gdb.sendline('b ExecutorStart')
        gdb.expect(gdb_prompt)
        logger.debug(gdb.before)

        gdb.sendline('c')
        # Don't expect prompt until the breakpoint hit.

        # have session 2 acquire the resource queue lock
        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""
            set role u;
            select 1;
        """)
        sql_script.flush()
        psql2.sendline(r'\i ' + sql_script.name)
        # Don't expect prompt until it's ready.

        # Now, GDB should hit the breakpoint.
        gdb.expect(gdb_prompt)
        logger.debug(gdb.before)

        # Session 1, expect prompt for role u
        logger.debug(psql1.before)
        psql1.expect(psqlu_prompt)

        # Cause session 1 to wait on a resource queue lock
        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""
            select 1
        """)
        sql_script.flush()
        psql1.sendline(r'\i ' + sql_script.name)
        # Don't expect prompt as session 1 is waiting

        # cancel session 1
        psql3 = pexpect.spawn('psql -n')
        psql3.expect(psql_prompt)

        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""
            select pg_cancel_backend
          """)
        sql_script.write('(%d);' % pid1)
        sql_script.flush()
        psql3.sendline(r'\i ' + sql_script.name)
        psql3.expect(psql_prompt)
        logger.debug(psql3.before)

        # make sure session 1 is cleanly terminate
        psql1.expect(psqlu_prompt)
        logger.debug(psql1.before)

        # start cleaup and exiting
        psql1.sendline(r'\q')
        psql1.close()

        # hit q to finish the rest of work in psql2.
        gdb.sendline('q')
        gdb.sendline('y')
        gdb.close()

        psql2.expect(psqlu_prompt)
        logger.debug(psql2.before)
        psql2.sendline(r'\q')
        psql2.close()

        sql_script = tempfile.NamedTemporaryFile()
        sql_script.write("""set client_min_messages = warning;
            drop table if exists foo;
            drop role u;
            drop resource queue q;""")
        sql_script.flush()
        psql3.sendline(r'\i ' + sql_script.name)
        # Don't expect prompt until it's ready.

        logger.debug("session 2 done")

        psql3.expect(psql_prompt)
        logger.debug(psql3.before)
        psql3.sendline(r'\q')
        psql3.close()

if __name__ == '__main__':
    main()
