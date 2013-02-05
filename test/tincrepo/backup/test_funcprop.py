from tinctest.models.gpdb.backup import BackupTestCase

class FuncPropDumpTest(BackupTestCase):
    def test_pg_dump01(self):
        """
        @author haradh1
        @description Checks if backup with pg_dump restores SQL-data access.
        @created 2013-01-28 00:00:00
        @modified 2013-01-28 00:00:00
        @tags mpp16160
        @gpdb_version [4.3, main]
        """

        self._run_test('test01', 'pg_dump')

    def test_gp_dump01(self):
        """
        @author haradh1
        @description Checks if backup with gp_dump restores SQL-data access.
        @created 2013-01-28 00:00:00
        @modified 2013-01-28 00:00:00
        @tags mpp16160
        @gpdb_version [4.3, main]
        """

        self._run_test('test01', 'gp_dump')
