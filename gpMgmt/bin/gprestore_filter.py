#!/usr/bin/env python

from gppylib.gpparseopts import OptParser, OptChecker
import sys

search_path_expr = 'SET search_path = '
search_path_start = 'S'
len_search_path_expr = len(search_path_expr)
copy_expr = 'COPY ' 
copy_start = 'C'
copy_expr_end = 'FROM stdin;\n'
len_copy_expr = len(copy_expr)
copy_end_expr = '\\.'
copy_end_start = '\\'


def get_table_schema_set(filename):
    dump_schemas = set()
    dump_tables = set()

    with open(filename) as fd:
        contents = fd.read()
        tables = contents.splitlines()
        for t in tables:
            parts = t.split('.')
            if len(parts) != 2:
                raise Exception("Bad table in filter list")
            schema = parts[0].strip()
            table = parts[1].strip()
            dump_tables.add((schema, table))
            dump_schemas.add(schema)

    return (dump_schemas, dump_tables)

def extract_schema(line):
    temp = line[len_search_path_expr:]
    idx = temp.find(",")
    if idx == -1:
        return None
    schema = temp[:idx]
    return schema

def extract_table(line):
    temp = line[len_copy_expr:]
    idx = temp.find(" ")
    if idx == -1:
        return None
    table = temp[:idx]
    return table

def process_data(dump_schemas, dump_tables, fdin, fdout):
    schema, table = None, None
    output = False
    #PYTHON PERFORMANCE IS TRICKY .... THIS CODE IS LIKE THIS BECAUSE ITS FAST
    for line in fdin:
        if (line[0] == search_path_start) and line.startswith(search_path_expr):
            schema = extract_schema(line)
            if schema and schema in dump_schemas:
                fdout.write(line)
        elif (line[0] == copy_start) and line.startswith(copy_expr) and line.endswith(copy_expr_end):
            table = extract_table(line)
            if table and (schema, table) in dump_tables:
                output = True
        elif output and (line[0] == copy_end_start) and line.startswith(copy_end_expr):
            table = None
            output = False
            fdout.write(line)

        if output:
            fdout.write(line)



if __name__ == "__main__":
    parser = OptParser(option_class=OptChecker)
    parser.remove_option('-h')
    parser.add_option('-h', '-?', '--help', action='store_true')
    parser.add_option('-t', '--tablefile', type='string', default=None)
    (options, args) = parser.parse_args()
    if not options.tablefile:
        raise Exception('-t table file name has to be specified')
    (schemas, tables) = get_table_schema_set(options.tablefile)
    process_data(schemas, tables, sys.stdin, sys.stdout)

