from tinctest.models.gpdb.backup import BackupTestCase

class FuncPropDumpTest(BackupTestCase):
    def test_pg_dump01(self):
        self._run_test('test01', 'pg_dump')

    def test_gp_dump01(self):
        self._run_test('test01', 'gp_dump')
