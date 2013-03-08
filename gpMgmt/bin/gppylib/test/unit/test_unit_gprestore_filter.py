#!/usr/bin/env python

import os, sys
import unittest2 as unittest
from gppylib import gplog
from mock import patch
from gppylib.mainUtils import ExceptionNoStackTraceNeeded
from gprestore_filter import get_table_schema_set, extract_schema, extract_table, \
                            process_data


logger = gplog.get_unittest_logger()

class GpRestoreFilterTestCase(unittest.TestCase):
 
    def test_get_table_schema_set00(self):
        fname = os.path.join(os.getcwd(), 'test1')
        with open(fname, 'w') as fd:
            fd.write('public.ao1\n')
            fd.write(' pepper.ao2   \n')

        (sc, tb) = get_table_schema_set(fname)
        self.assertEquals(sc, set(['public', 'pepper']))
        self.assertEquals(tb, set([('public','ao1'), ('pepper','ao2')]))

        os.remove(fname)

    def test_get_table_schema_set01(self):
        fname = os.path.join(os.getcwd(), 'test1')
        with open(fname, 'w') as fd:
            fd.write('publicao1\n')
            fd.write(' pepper.ao2   \n')

        with self.assertRaisesRegexp(Exception, "Bad table in filter list"):
            get_table_schema_set(fname)

        os.remove(fname)
    
    def test_get_table_schema_set02(self):
        fname = os.path.join(os.getcwd(), 'test1')
        with open(fname, 'w') as fd:
            fd.write('')

        (sc, tb) = get_table_schema_set(fname)
        self.assertEquals(sc, set())
        self.assertEquals(tb, set())

        os.remove(fname)
        
    def test_extract_schema00(self):
        line = 'SET search_path = pepper, pg_catalog;'
        schema = extract_schema(line)
        self.assertEquals(schema, 'pepper')

    def test_extract_schema01(self):
        line = 'SET search_path = pepper pg_catalog;'
        schema = extract_schema(line)
        self.assertEquals(schema, None)

    def test_extract_table00(self):
        line = 'COPY ao_table (column1, column2, column3) FROM stdin;'
        table = extract_table(line)
        self.assertEqual(table, 'ao_table')

    def test_extract_table01(self):
        line = 'COPYao_table(column1column2column3)FROMstdin;'
        table = extract_table(line)
        self.assertEqual(table, None)

    def test_process_data00(self):

        test_case_buf = """
--
-- Greenplum Database database dump
--

SET statement_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

SET search_path = pepper, pg_catalog;

SET default_with_oids = false;

--
-- Data for Name: ao_table; Type: TABLE DATA; Schema: pepper; Owner: dcddev
--

COPY ao_table (column1, column2, column3) FROM stdin;
3	backup	2010-01-04
7	backup	2010-01-08
11	backup	2010-01-12
15	backup	2010-01-16
19	backup	2010-01-20
23	backup	2010-01-24
\.


--
-- Greenplum Database database dump complete
--
"""

        expected_out = """SET search_path = pepper, pg_catalog;
COPY ao_table (column1, column2, column3) FROM stdin;
3	backup	2010-01-04
7	backup	2010-01-08
11	backup	2010-01-12
15	backup	2010-01-16
19	backup	2010-01-20
23	backup	2010-01-24
\.
"""

        in_name = os.path.join(os.getcwd(), 'infile')
        out_name = os.path.join(os.getcwd(), 'outfile')
        with open(in_name, 'w') as fd:
            fd.write(test_case_buf)

        dump_schemas = set(['pepper'])
        dump_tables = set([('pepper', 'ao_table')])
        with open(out_name, 'w') as fdout:
            with open(in_name, 'r') as fdin:
                process_data(dump_schemas, dump_tables, fdin, fdout)

        with open(out_name, 'r') as fd:
            results = fd.read()

        self.assertEquals(results, expected_out)
        os.remove(in_name)
        os.remove(out_name)
        
       

    def test_process_data01(self):

        test_case_buf = """
--
-- Greenplum Database database dump
--

SET statement_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

SET search_path = public, pg_catalog;

SET default_with_oids = false;

--
-- Data for Name: ao_index_table; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_index_table (column1, column2, column3) FROM stdin;
3	backup	2010-01-04
7	backup	2010-01-08
11	backup	2010-01-12
15	backup	2010-01-16
1091	restore	2012-12-27
\.


--
-- Data for Name: ao_part_table; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table (column1, column2, column3) FROM stdin;
\.


--
-- Data for Name: ao_part_table_1_prt_p1; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_1_prt_p1 (column1, column2, column3) FROM stdin;
\.


--
-- Data for Name: ao_part_table_1_prt_p1_2_prt_1; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_1_prt_p1_2_prt_1 (column1, column2, column3) FROM stdin;
2	backup	2010-01-03
6	backup	2010-01-07
10	backup	2010-01-11
14	backup	2010-01-15
18	backup	2010-01-19
22	backup	2010-01-23
26	backup	2010-01-27
30	backup	2010-01-31
34	backup	2010-02-04
361	backup	2010-12-28
\.


--
-- Data for Name: ao_part_table_1_prt_p1_2_prt_2; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_1_prt_p1_2_prt_2 (column1, column2, column3) FROM stdin;
365	backup	2011-01-01
369	backup	2011-01-05
719	backup	2011-12-21
723	backup	2011-12-25
727	backup	2011-12-29
\.

--
-- Data for Name: ao_part_table_comp; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_comp (column1, column2, column3) FROM stdin;
\.


--
-- Data for Name: ao_part_table_comp_1_prt_p1; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_comp_1_prt_p1 (column1, column2, column3) FROM stdin;
\.


--
-- Data for Name: ao_part_table_comp_1_prt_p1_2_prt_1; Type: TABLE DATA; Schema: public; Owner: dcddev
--

COPY ao_part_table_comp_1_prt_p1_2_prt_1 (column1, column2, column3) FROM stdin;
1	backup	2010-01-02
5	backup	2010-01-06
9	backup	2010-01-10
13	backup	2010-01-14
17	backup	2010-01-18
1063	restore	2012-11-29
1067	restore	2012-12-03
1071	restore	2012-12-07
1075	restore	2012-12-11
1079	restore	2012-12-15
1083	restore	2012-12-19
1087	restore	2012-12-23
1091	restore	2012-12-27
\.


--
-- Greenplum Database database dump complete
--
"""

        expected_out = """SET search_path = public, pg_catalog;
COPY ao_part_table_1_prt_p1_2_prt_1 (column1, column2, column3) FROM stdin;
2	backup	2010-01-03
6	backup	2010-01-07
10	backup	2010-01-11
14	backup	2010-01-15
18	backup	2010-01-19
22	backup	2010-01-23
26	backup	2010-01-27
30	backup	2010-01-31
34	backup	2010-02-04
361	backup	2010-12-28
\.
COPY ao_part_table_comp_1_prt_p1_2_prt_1 (column1, column2, column3) FROM stdin;
1	backup	2010-01-02
5	backup	2010-01-06
9	backup	2010-01-10
13	backup	2010-01-14
17	backup	2010-01-18
1063	restore	2012-11-29
1067	restore	2012-12-03
1071	restore	2012-12-07
1075	restore	2012-12-11
1079	restore	2012-12-15
1083	restore	2012-12-19
1087	restore	2012-12-23
1091	restore	2012-12-27
\.
"""

        in_name = os.path.join(os.getcwd(), 'infile')
        out_name = os.path.join(os.getcwd(), 'outfile')
        with open(in_name, 'w') as fd:
            fd.write(test_case_buf)

        dump_schemas = set(['public'])
        dump_tables = set([('public', 'ao_part_table_comp_1_prt_p1_2_prt_1'), ('public', 'ao_part_table_1_prt_p1_2_prt_1')])
        with open(out_name, 'w') as fdout:
            with open(in_name, 'r') as fdin:
                process_data(dump_schemas, dump_tables, fdin, fdout)

        with open(out_name, 'r') as fd:
            results = fd.read()

        self.assertEquals(results, expected_out)
        os.remove(in_name)
        os.remove(out_name)
        
       
    def test_process_data03(self):

        test_case_buf = """
COPY ao_table (column1, column2, column3) FROM stdin;
3	backup	2010-01-04
7	backup	2010-01-08
11	backup	2010-01-12
15	backup	2010-01-16
19	backup	2010-01-20
23	backup	2010-01-24
\.
"""
        expected_out = ''
        in_name = os.path.join(os.getcwd(), 'infile')
        out_name = os.path.join(os.getcwd(), 'outfile')
        with open(in_name, 'w') as fd:
            fd.write(test_case_buf)

        dump_schemas = set(['public'])
        dump_tables = set([('public', 'ao_table')])
        with open(out_name, 'w') as fdout:
            with open(in_name, 'r') as fdin:
                process_data(dump_schemas, dump_tables, fdin, fdout)

        with open(out_name, 'r') as fd:
            results = fd.read()

        self.assertEquals(results, expected_out)
        os.remove(in_name)
        os.remove(out_name)
        
 
    def test_process_data04(self):

        test_case_buf = """
--
-- Greenplum Database database dump
--

SET statement_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = off;
SET check_function_bodies = false;
SET client_min_messages = warning;
SET escape_string_warning = off;

SET search_path = pepper, pg_catalog;

SET default_with_oids = false;

--
-- Data for Name: ao_table; Type: TABLE DATA; Schema: pepper; Owner: dcddev
--

 COPY ao_table (column1, column2, column3) FROM stdin;
3	backup	2010-01-04
7	backup	2010-01-08
11	backup	2010-01-12
15	backup	2010-01-16
19	backup	2010-01-20
23	backup	2010-01-24
\.


--
-- Greenplum Database database dump complete
--
"""

        expected_out = """SET search_path = pepper, pg_catalog;
"""

        in_name = os.path.join(os.getcwd(), 'infile')
        out_name = os.path.join(os.getcwd(), 'outfile')
        with open(in_name, 'w') as fd:
            fd.write(test_case_buf)

        dump_schemas = set(['pepper'])
        dump_tables = set([('pepper', 'ao_table')])
        with open(out_name, 'w') as fdout:
            with open(in_name, 'r') as fdin:
                process_data(dump_schemas, dump_tables, fdin, fdout)

        with open(out_name, 'r') as fd:
            results = fd.read()

        self.assertEquals(results, expected_out)
        os.remove(in_name)
        os.remove(out_name)
              

