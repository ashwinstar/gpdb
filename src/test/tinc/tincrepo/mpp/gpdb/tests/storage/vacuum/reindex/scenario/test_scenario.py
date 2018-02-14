"""
Copyright (c) 2004-Present Pivotal Software, Inc.

This program and the accompanying materials are made available under
the terms of the under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

from mpp.models import SQLTestCase
from mpp.gpdb.tests.storage.GPDBStorageBaseTestCase import *
from mpp.lib.filerep_util import Filerepe2e_Util
from mpp.gpdb.tests.storage.vacuum.reindex import Reindex

@tinctest.skipLoading("Test model. No tests loaded.")
class reindex_stp(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'reindex_stp/sql/'
    ans_dir = 'reindex_stp/expected/'
    out_dir = 'reindex_stp/output/'

@tinctest.skipLoading("Test model. No tests loaded.")
class reindex_db(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'reindex_db/sql/'
    ans_dir = 'reindex_db/expected/'
    out_dir = 'reindex_db/output/'

    def setUp(self):
        super(reindex_db, self).setUp()
        util = Filerepe2e_Util()
        util.inject_fault(f="reindex_db", y="suspend", r="primary", seg_id=1)

@tinctest.skipLoading("Test model. No tests loaded.")
class drop_obj(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'drop_obj/sql/'
    ans_dir = 'drop_obj/expected/'
    out_dir = 'drop_obj/output/'

    def setUp(self):
        super(drop_obj, self).setUp()
        self.util = Filerepe2e_Util()
        self.util.check_fault_status(fault_name="reindex_db", status="triggered", seg_id=1, max_cycle=20)

    def tearDown(self):
        self.util.inject_fault(y="reset", f="reindex_db", r="primary", seg_id=1)

@tinctest.skipLoading("Test model. No tests loaded.")
class reindex_rel(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'reindex_rel/sql/'
    ans_dir = 'reindex_rel/expected/'
    out_dir = 'reindex_rel/output/'

    def setUp(self):
        super(reindex_rel, self).setUp()
        self.util = Filerepe2e_Util()
        self.util.inject_fault(f="reindex_relation", y="suspend", r="primary", seg_id=1)

@tinctest.skipLoading("Test model. No tests loaded.")
class add_index(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'add_index/sql/'
    ans_dir = 'add_index/expected/'
    out_dir = 'add_index/output/'

    def setUp(self):
        super(add_index, self).setUp()
        self.util = Filerepe2e_Util()
        self.util.check_fault_status(fault_name="reindex_relation",status="triggered", seg_id=1, max_cycle=20)

    def tearDown(self):
        self.util.inject_fault(y="reset", f="reindex_relation", r="primary", seg_id=1)

@tinctest.skipLoading("Test model. No tests loaded.")
class reindex_verify(SQLTestCase):
    """
    @gucs gp_create_table_random_default_distribution=off
    """

    sql_dir = 'reindex_verify/sql/'
    ans_dir = 'reindex_verify/expected/'
    out_dir = 'reindex_verify/output/'

    def setUp(self):
        super(reindex_verify, self).setUp()
        self.util = Reindex()

    def tearDown(self):
        tinctest.logger.info("Starting gpcheckcat...")
        self.util.do_gpcheckcat()
