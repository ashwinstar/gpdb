#!/usr/bin/env python
'''
   Greenplum Map/Reduce Driver
'''

#============================================================
import sys, os, optparse, re, subprocess
try:
    import yaml
except:
    sys.stderr.write('Please install the PyYAML module and try again')
    sys.stderr.write('http://pyyaml.org')
    exit(1)

#============================================================
def pretty_version(v):
    '''
    Turns the RCS revision number into a prettier format
    '''
    m = re.compile('Revision: #(\d+)').search(v)
    if m: return str(1+float(m.group(1))/100)
    else: return str(v)
__version__ = pretty_version('$Revision: #6 $')

#============================================================
class InternalError(StandardError): pass
class ParseError(StandardError): pass
class SyntaxError(ParseError): pass

#============================================================
def Lookup(myDict, key, default=None):
    '''
    Description: 
      Dictionary lookup function that supports default values and custom 
      exceptions
    Parameters:
      myDict         is type dict
      key            is type str
      default=None   is type any (special behavior for Exception)
    Exceptions:
      if default is an Exception it will be raised on lookup failure
    Returns:
      myDict[key] or default
    '''
    if key not in myDict: 
        if isinstance(default, Exception):
            raise default
        return default
    return myDict[key]

#============================================================
class MRObject:
    '''
    Abstract Map/Reduce object class
    '''
    type  = 'Object'

    def __init__(self, parse):
        self.name = Lookup(parse, 'NAME', 'Anonymous')
        self.parse = parse
        self.value = {}
        self.depend_field = {}
        self.kind_field = {}
        self.depend = {}
        self.required = set()
        self.valid = {}

        # Required by all objects
        self.Required('NAME')
        
    def __str__(self):
        return '%s (%s): %s' % (self.type, self.name, str(self.value))

    def TypeCheck(self, typename):
        return typename == 'Object'

    def SQLCreate(self):
        'Abstract method'
        return ''

    def SQLDrop(self):
        'Abstract method'
        return ''

    def Required(self, field, depend=None, kind='string'):
        self.required.add(field)
        self.valid[field] = None
        self.kind_field[field] = type(kind)
        if depend: 
            self.depend_field[field]=depend

    def Optional(self, field, default=None, depend=False, kind=None):
        if field in self.required:
            raise InternalError(self.type + ' redefines %s as optional' % field)
        self.valid[field] = default
        if kind != None:
            self.kind_field[field] = type(kind)
        elif default != None:
            self.kind_field[field] = type(default)
        else:
            self.kind_field[field] = type('string')
        if depend:
            self.depend_field[field]=depend

    def CheckInput(self, objects):

        # Check that all required fields are present
        errstr = '%s %s missing ' % (self.type, self.name)
        map(lambda(x): Lookup(self.parse, x, ParseError(errstr + x)),
            self.required)
        
        # Check that no invalid fields are present
        errstr = '%s (%s) has unknown attribute ' % (self.type, self.name)
        for field in self.parse:
            if field not in self.valid:
                raise ParseError(errstr + field)

        # Check field types and assign all fields
        for field in self.valid:
            value = Lookup(self.parse, field, default=self.valid[field])
            self.value[field] = self.CoerceType(value, self.kind_field[field])

        # Find any objects on which we have dependencies
        for field in self.depend_field:
            name = self.value[field]
            if name:
                name = name.upper()
                dependtype = self.depend_field[field]
                self.depend[name] = Lookup(objects, name.upper())
                if self.depend[name] == None:
                    print '[WARN] %s.%s %s not listed in YAML' % \
                        (self.name, dependtype, name)
                elif not self.depend[name].TypeCheck(dependtype):
                    print '[WARN] %s.%s type mismatch %s/%s' % \
                        (self.name, name, dependtype, self.depend[name].type)

    def CheckInput2(self, objects):
        pass

    def CoerceType(self, value, vtype):
        if value == None or type(value) == vtype:
            return value

        # int -> string conversions
        if type(value) == type(1) and vtype == type('string'):
            return str(value)

        # String -> array conversions
        if type(value) == type('string') and vtype == type([]):
            return [value]

        # array -> string conversion
        if type(value) == type('string') and vtype == type([]):
            if len(value) == 0:
                return ''
            if type(value[0]) == type('string'):
                return ', '.join(value)        

        # If we can't figure out a conversion giveup
        errstr = '%s (%s) has attribute type error on ' % (self.type, self.name)
        raise ParseError(errstr + str(value))


#============================================================
class Input(MRObject): 
    'Greenplum Map/Reduce Input specification object'
    type      = 'Input'

    def __init__(self, parse):
        MRObject.__init__(self, parse)
        self.Optional('FILE', kind=[]);
        self.Optional('QUERY');
        self.Optional('TABLE');
        self.Optional('SPLITTER', depend='Splitter')
        self.Optional('DELIMITER')
        self.Optional('COLUMNS', default=['value text'])
        self.Optional('FORMAT', default='text')

    def TypeCheck(self, typename):
        return typename == 'Input' or MRObject.TypeCheck(self, typename)

    def CheckInput(self, objects):
        MRObject.CheckInput(self, objects)

        file  = Lookup(self.value, 'FILE')
        query = Lookup(self.value, 'QUERY')
        table = Lookup(self.value, 'TABLE')

        if ((file and (query or table)) or (query and table)):
            raise ParseError('Input %s may only specify one of FILE/TABLE/QUERY'
                             % self.name)

        if ((query or table) and Lookup(self.value, 'COLUMNS')):
            raise ParseError('Only inputs of type FILE may specify COLUMNS')
        if ((query or table) and Lookup(self.value, 'FORMAT')):
            raise ParseError('Only inputs of type FILE may specify FORMAT')
        if ((query or table) and Lookup(self.value, 'DELIMITER')):
            raise ParseError('Only inputs of type FILE may specify DELIMITER')

        # Check valid file URIs
        if file:
            for f in file:
                m = re.compile('(\w+):(.*)').match(f)
                if not m:
                    raise ParseError('Invalid file specification: ' + f)


    def SQLDrop(self):
        if Lookup(self.value, 'FILE'):
            return 'DROP EXTERNAL TABLE IF EXISTS %(NAME)s CASCADE;\n' % self.value

    def SQLCreate(self):
        files = Lookup(self.value, 'FILE')
        if files:
            sql  = 'CREATE EXTERNAL TABLE %(NAME)s(' % self.value
            sql += ', '.join(self.value['COLUMNS'])
            sql += ")\n  LOCATION(";
            for i in range(0,len(files)):
                if (i > 0):
                    sql += ",\n           "
                m = re.compile('(\w+):(.*)').match(files[i])
                sql += "'file://%s%s'" % (m.group(1), m.group(2))
            sql += ")\n  FORMAT '%(FORMAT)s'" % self.value
            if self.value['DELIMITER'] != None:
                sql += " (DELIMITER '%s')" % self.value['DELIMITER']
            sql += ';\n'
            return sql

    def QueryAppend(self, qry):
        if qry.sql or qry.columns:
            raise InternalError('Input must be the root of query tree: ' +
                                self.name)
        # Columns should all be <name, type> pairs, extract the names
        qry.columns = []
        for c in self.value['COLUMNS']:
            name = c.split()[0].lower()
            qry.columns.append(name)
        qry.sql     = self.name

#============================================================
class Output(MRObject):
    'Greenplum Map/Reduce Output specification object'    
    type = 'Output'

    def __init__(self, parse):
        MRObject.__init__(self, parse)

    def TypeCheck(self, typename):
        return typename == 'Output' or MRObject.TypeCheck(self, typename)

    def QueryAppend(self, qry):
        if not qry.sql or not qry.columns:
            raise InternalError('Output.QueryAppend() must be over source')

        columns = ', '.join(qry.columns)
        if qry.depth > 1:
            source = qry.sql
        else:
            source  = 'SELECT %s\n' % columns
#            source += 'FROM   (%s) q%s' % (qry.sql, qry.depth)
            source += 'FROM   %s' % qry.sql
        if qry.explain:
            qry.sql = 'EXPLAIN\n%s\nORDER BY %s;\n' % (source, columns)
        else:
            qry.sql = '%s\nORDER BY %s;\n' % (source, columns)


#============================================================
class Splitter(MRObject):
    '''
    Notes:
       We could create a standard common ancestor class between
       Splitter and Function, but it doesn't really seam worth it.
    '''
    type = 'Splitter'

    def __init__(self, parse):
        MRObject.__init__(self, parse)
        self.Required('FUNCTION')
        self.Optional('LANGUAGE', default='perl')

    def TypeCheck(self, typename):
        return typename == 'Splitter' or MRObject.TypeCheck(self, typename)

#============================================================
class Function(MRObject):
    type = 'Function'

    def __init__(self, parse):
        MRObject.__init__(self, parse)
        self.Required('FUNCTION')
        self.Optional('LANGUAGE', default='perl')
        self.Optional('OPTIMIZE', default='')

    def TypeCheck(self, typename):
        return typename == 'Function' or MRObject.TypeCheck(self, typename)

    def CheckInput(self, objects):
        MRObject.CheckInput(self, objects)

        # Every parameter should be either:
        #   <name type> or <name>
        p_types = []
        for p in self.value['PARAMETERS']:
            sp = p.split();
            if len(sp) == 1:
                p_name = sp[0]
                p_type = 'text'
                self.value['PARAMETERS'][p] = p_name + ' text'
            elif len(sp) == 2:
                p_name = sp[0]
                p_type = sp[1]
            else:
                errstr = '%s (%s) has bad PARAMETERS: %s' % \
                    (self.type, self.name, p)
                raise ParseError(errstr)
            p_types.append(p_type)
        self.value['TYPES'] = p_types

        if self.value['LANGUAGE'].lower() == 'perl':
            self.value['LANGUAGE'] = 'plperl'
        if self.value['LANGUAGE'].lower() == 'python':
            self.value['LANGUAGE'] = 'plpythonu'
        if self.value['LANGUAGE'].lower() == 'R':
            self.value['LANGUAGE'] = 'plr'

        # If there is more than one return value then we will need to construct
        # a compound return type (bleh)
        if len(self.value['RETURNS']) > 1:
            self.typed = True
            self.value['RTYPE'] = 'MapReduceType_'+self.name
        elif len(self.value['RETURNS']) == 1:
            self.typed = False
            self.value['RTYPE'] = self.value['RETURNS'][0]
        else:
            errstr = '%s (%s) has empty RETURN list' % (self.type, self.name)
            raise InternalError(errstr)

    def SQLCreate(self):
        sql = ''
        if self.typed:
            rtypes = ', '.join(self.value['RETURNS'])
            sql += 'CREATE TYPE %s as (%s);\n' % (self.value['RTYPE'], rtypes)
        params = ', '.join(self.value['PARAMETERS'])
        sql += 'CREATE FUNCTION %s(%s)\n' % (self.name, params)
        sql += 'RETURNS %s AS $$\n' % self.value['RTYPE']
        sql += self.value['FUNCTION']
        sql += '\n$$ LANGUAGE %s' % self.value['LANGUAGE']
        sql += ' %s;\n' % self.value['OPTIMIZE']
        return sql
        
    def SQLDrop(self):
        types = ', '.join(self.value['TYPES'])
        sql = 'DROP FUNCTION IF EXISTS %s(%s) CASCADE;\n' % (self.name, types)
        if self.typed:
            sql += 'DROP TYPE IF EXISTS MapReduceType_%s CASCADE;\n' % self.name
        return sql

#============================================================
class Mapper(Function): 
    type = 'Mapper'

    def __init__(self, parse):
        Function.__init__(self, parse)
        self.Optional('PARAMETERS', default=['key text', 'value text'])
        self.Optional('RETURNS', default=['key text', 'value text'])

    def TypeCheck(self, typename):
        return typename == 'Mapper' or Function.TypeCheck(self, typename)

    def QueryAppend(self, qry):
        if not qry.sql and qry.columns:
            raise InternalError('Mapper.QueryAppend() must be over source:'
                                + self.name)

        # If the query depth is > 1 then the starting query is a full
        # subselect and we need to wrap it in parens
        if qry.depth > 1:
            source = '(%s)' % qry.sql
        else:
            source = qry.sql

        # We use query depth to name our subqueries
        level = 'q' + str(qry.depth)
        qry.depth = qry.depth + 1

        # If there is a key column in the source then we use that as
        # the key, otherwise get a value from the input keys.
        if 'key' in qry.columns:   key = 'key'
        elif qry.keys:             key = "'%s'" % qry.keys[0]
        else:                      key = "''"

        # Build up the select columns
        columns = []
        ctypes  = {}
        for p in self.value['RETURNS']:
            sp = p.split()
            columns.append(sp[0])
            ctypes[sp[0]] = sp[1]
        colstr = '(m), '.join(columns) + '(m)'

        # Build up the parameters to pass to the function
        params = []
        ptypes = {}
        for p in self.value['PARAMETERS']:
            sp = p.split()
            if sp[0] == 'key':
                params.append(key)
            else:
                params.append(sp[0])
            ptypes[sp[0]] = sp[1]
        paramstr = ', '.join(params)

        # Put it all together
        qry.sql  = 'SELECT %s\n' % colstr
        qry.sql += 'FROM (SELECT %s(%s) as m\n' % (self.name, paramstr)
        qry.sql += '      FROM %s) %s\n' % (source, level)
        qry.sql += 'WHERE m is not null'

        # And mark what columns exist from this mapping
        qry.columns = columns

#============================================================
class Reducer(MRObject): 
    type = 'Reducer'

    def __init__(self, parse):
        MRObject.__init__(self, parse)
        self.Required('TRANSITION', depend='Function')
        self.Optional('COMBINE', depend='Function')
        self.Optional('FINALIZE', depend='Function')
        self.Optional('INITIALIZE', default="''")

    def TypeCheck(self, typename):
        return typename == 'Reducer' or MRObject.TypeCheck(self, typename)

    def CheckInput2(self, objects):
        # Derive parameters from functions and verify that they all 
        # play well together.
        name = self.value['TRANSITION'].upper()
        transition = self.depend[name]
        self.value['PARAMETERS'] = transition.value['PARAMETERS'][1:]
        self.value['RETURNS']    = transition.value['RETURNS'][:]
        self.value['STATETYPE']  = transition.value['RTYPE'][:]

        # FIXME: validate parameters of combine
        if Lookup(self.value, 'COMBINE'):
            name = self.value['COMBINE'].upper()
            combine = self.depend[name]

        # FIXME: validate parameters of finalize
        if Lookup(self.value, 'FINALIZE'):
            name = self.value['FINALIZE'].upper()
            finalize = self.depend[name]

    def SQLCreate(self):

        # Aggregates don't get named parameters
        types = map(lambda(x): x.split()[1], self.value['PARAMETERS'])
        typestr = ', '.join(types)
        
        sql  = 'CREATE AGGREGATE %s(%s) (\n' % (self.name, typestr)
        if self.value['FINALIZE']:
            sql += '  finalfn=%(FINALIZE)s,\n' % self.value
        if self.value['COMBINE']:
            sql += '  prefunc=%(COMBINE)s,\n' % self.value
        sql += '  initcond=%(INITIALIZE)s,\n' % self.value
        sql += '  stype=%(STATETYPE)s,\n'     % self.value
        sql += '  sfunc=%(TRANSITION)s\n);\n' % self.value
        return sql

    def SQLDrop(self):

        # Aggregates don't get named parameters
        types = map(lambda(x): x.split()[1], self.value['PARAMETERS'])
        typestr = ', '.join(types)

        return 'DROP AGGREGATE IF EXISTS %s(%s) CASCADE;\n' \
            % (self.name, typestr)

    def QueryAppend(self, qry):
        if not qry.sql and qry.columns:
            raise InternalError('Reducer.QueryAppend() must be over source:'
                                + self.name)

        # If the query depth is > 1 then the starting query is a full
        # subselect and we need to wrap it in parens
        if qry.depth > 1:
            source = '(%s) q%s' % (qry.sql, str(qry.depth))
            qry.depth = qry.depth+1
        else:
            source = qry.sql

        # Decide which columns to pass to the reducer and which to group by
        params = map(lambda(x): x.split()[0], self.value['PARAMETERS'])
        param   = ', '.join(params)

        # Unless otherwise specified we group over anything we didn't
        # take as input.
        groupcols = qry.columns
        for c in params:
            if c in groupcols:
                groupcols.remove(c)
        groupby = ', '.join(groupcols)
        rname = self.value['RETURNS'][0].split()[0]

        # Put it all together
        qry.sql  = 'SELECT %s, %s(%s) as %s\n' \
            % (groupby, self.name, param, rname)
        qry.sql += 'FROM %s\n' % source
        qry.sql += 'GROUP BY %s' % groupby

        # and tweak the returned columns
        groupcols.append(rname)
        qry.columns = groupcols


#============================================================
class Transition(Function): 
    type = 'Transition'

    def __init__(self, parse):
        Function.__init__(self, parse)
        self.Optional('PARAMETERS', default=['state text', 'value text'])
        self.Optional('RETURNS', default=[None])

    def TypeCheck(self, typename):
        return typename == 'Transition' or Function.TypeCheck(self, typename)

    def CheckInput(self, objects):
        Function.CheckInput(self, objects)

        # We defaulted to an empty returns array, and fill it in as the
        # first value from the parameters array
        if self.value['RETURNS'][0] == None:
            rtype = self.value['PARAMETERS'][0].split()[1]
            self.value['RETURNS'] = ['value ' + rtype]
            self.value['RTYPE'] = rtype


#============================================================
class Combiner(Function): 
    type = 'Combiner'

    def __init__(self, parse):
        Function.__init__(self, parse)
        self.Optional('PARAMETERS', default=['text state1', 'text state2'])
        self.Optional('RETURNS', default=['text'])

    def TypeCheck(self, typename):
        return typename == 'Combiner' or Function.TypeCheck(self, typename)

#============================================================
class Finalizer(Function): 
    type = 'Finalizer'

    def __init__(self, parse):
        Function.__init__(self, parse)
        self.Optional('PARAMETERS', default=['state text'])
        self.Optional('RETURNS', default=['text'])

    def TypeCheck(self, typename):
        return typename == 'Finalizer' or Function.TypeCheck(self, typename)

#============================================================
class Task(Input): 
    type = 'Task'

    def __init__(self, parse):
        MRObject.__init__(self, parse)
        self.Required('SOURCE', depend='Input')
        self.Optional('MAPPER',  default='IDENTITY', depend='Mapper')
        self.Optional('REDUCER', default='IDENTITY', depend='Reducer')
        self.Optional('TARGET',  default='STDOUT', depend='Output')

    def TypeCheck(self, typename):
        return typename == 'Task' or Input.TypeCheck(self, typename)
        
    def GenerateQuery(self, keys=[], explain=False):
        q = MRQuery(keys, explain)
        for x in ['SOURCE', 'MAPPER', 'REDUCER', 'TARGET']:
            objname = self.value[x].upper()
            obj = self.depend[objname]
            obj.QueryAppend(q)
        return q.sql


#============================================================
class Identity(Mapper,Reducer):
    '''
    Because IDENTITY can be used as either a mapper or reducer it gets 
    its own special class that inherits from both.
    '''
    type = 'Identity'
    def __init__(self):
        MRObject.__init__(self, {'NAME': 'IDENTITY'})
    def TypeCheck(self, typename):
        return typename == 'Identity' or \
            Mapper.TypeCheck(self, typename) or \
            Reducer.TypeCheck(self, typename)
    def CheckInput(self, objects):
        pass
    def SQLCreate(self):
        return ''
    def SQLDrop(self):
        return ''
    def QueryAppend(self, q):
        pass

class MRQuery:
    '''
    A class abstracting a Map/Reduce query stream
    '''
    def __init__(self, keys, explain):
        self.keys    = keys
        self.explain = explain
        self.depth   = 1
        self.columns = None
        self.sql     = None


#============================================================
class Document:
    '''
    A map/reduce document is a collector class that holds a set of 
    inputs, outputs, mappers, and reducers.  
    
    It parses from and serializes to a YAML document.
    '''
    
    parseobjects = {
        'INPUT':       lambda(x): Input(x),
        'SPLITTER':    lambda(x): Splitter(x),
        'MAP':         lambda(x): Mapper(x),
        'REDUCER':     lambda(x): Reducer(x),
        'TRANSITION':  lambda(x): Transition(x),
        'COMBINE':     lambda(x): Combiner(x),
        'FINALIZE':    lambda(x): Finalizer(x),
        'TASK':        lambda(x): Task(x),
        }
    
    def __init__(self, options, args):
        cmd = subprocess.Popen('id -un', shell=True, stdout=subprocess.PIPE)
        id = cmd.communicate()[0].strip()

        self.objects = {
            'IDENTITY': Identity(), 
            'STDOUT':   Output({'NAME': 'STDOUT'})
            }
        self.tasks = []
        self.parsetree = None
        self.version   = None
        
        # First stage parse as YAML
        if len(args) > 0: 
            file = args[0]
            self.keys = args[1:]
        else: 
            file=None
            self.keys=None

        self.YAMLParseFile(file)

        # Second stage domain specific parse
        if 'VERSION' not in self.parsetree:
            raise ParseError('File %s did not specify VERSION' % file)
        self.version = self.parsetree['VERSION']
        self.user = Lookup(self.parsetree, 'USER', id)
        self.db   = Lookup(self.parsetree, 'DATABASE', self.user)

        # Get the object definitions
        defines   = Lookup(self.parsetree, 'DEFINE', [])
        for elem in defines:
            if len(elem) > 1:
                raise SyntaxError('Too many elements at:\n' + str(elem))
            rtype = elem.keys()[0].upper()
            if rtype not in self.parseobjects:
                raise SyntaxError('Unknown type: ' + rtype)
            self.AddObject(self.parseobjects[rtype](elem[rtype]))

        # Get the list of tasks to run
        tasks = Lookup(self.parsetree, 'EXECUTE', [])
        count = 1
        for task in tasks:
            if len(task) > 1:
                raise SyntaxError('Too many Elements at:\n' + str(elem))
            rtype = task.keys()[0].upper()
            if rtype != 'RUN':
                raise SyntaxError('Unknown execution type: ' + rtype)
            if type(task['RUN']) == type('string'):
                task['RUN'] = {'SOURCE': task['RUN']};
            task['RUN']['NAME'] = 'Anonymous_' + str(count)
            self.tasks.append(Task(task['RUN']))
        
        # Third stage, check objects and dependencies
        for name in self.objects:
            self.objects[name].CheckInput(self.objects)
        for task in self.tasks:
            task.CheckInput(self.objects)

        # Final stage checks that require depencies
        for name in self.objects:
            self.objects[name].CheckInput2(self.objects)

    def __str__(self):
        '''
        Reconvert to YAML
        '''
  
        # FIXME
        return '\n'.join(map(str, self.objects.values()))

    def YAMLParseFile(self, doc):
        '''
        Parse a YAML file
        '''        
        file = open(doc, 'r')
        self.parsetree = yaml.load(file)
        file.close()
        return
                
    def AddObject(self, newObject):
        '''
        Description:  Adds a parsed Object to the Document
        Parameters:   newObject is type Object
        Returns:      None
        Exceptions:   ParseError on name collision
        '''
        name = newObject.name.upper()

        # Check if there is a name conflict
        if name in self.objects:
            raise ParseError('Name collision on: ' + newObject.name)

        self.objects[name] = newObject
        return None

    def WalkDependencyGraph(self, func):
        '''
        Walks the object dependency graph from the bottom up calling the 
        specified function on every object.
        '''
        objnames = set(self.objects)
        while len(objnames) > 0:

            # Find names with no other dependencies in objnames
            nodep = set()
            for o in objnames:
                dep = set(self.objects[o].depend)
                if len(dep & objnames) == 0:
                    func(self.objects[o])
                    nodep.add(o)

            # Given the format of the YAML schema it should be impossible
            # to create circular dependencies if the typechecking has
            # been honored.  So it is an Internal error if there are
            # objects left but none with no depencies outside the current
            # working set.
            if (len(nodep) == 0):
                raise InternalError('Circular object dependencies between: '
                                    + str(objnames))

            # Remove the processe objects and continue
            objnames = objnames - nodep

    def GenerateSQL(self, explain=False):
        '''
        '''
        
        self.create = '--- CREATING OBJECTS ---\n'
        self.drop = ''
        def sqlgen(x):
            c = x.SQLCreate()
            d = x.SQLDrop()
            if c: self.create += c + "\n"
            if d: self.drop    = d + self.drop
        self.WalkDependencyGraph(sqlgen)
        sql  = self.create 
        sql += "--- RUN QUERIES\n"
        for t in self.tasks:
            sql += t.GenerateQuery(keys=self.keys, explain=explain) + '\n'
        sql += '--- CLEANUP OBJECTS ---\n'
        sql += self.drop

        return sql

    def ExplainSQL(self):
        return self.ExecuteSQL(explain=True)

    def ExecuteSQL(self, explain=False):
        sql  = self.GenerateSQL(explain)
        psql = subprocess.Popen('psql -qtX -U %s %s' % (self.user, self.db),
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                stdin=subprocess.PIPE, shell=True)
        out, err = psql.communicate(sql)
        if not out:
            out = ''
        if not err:
            err = ''
        else:
            print 'ERROR: ' + str(err)

        # Remove "NOTICE" warnings on "DROP IF EXISTS"
        return re.sub("NOTICE: .*\n", "", out)
        

if __name__ == '__main__':
    cmdline = optparse.OptionParser(version='%prog ' + __version__)
    cmdline.add_option('--print', action='store_const', const='print',
                       dest='mode', help='Print the generated SQL and exit')
    cmdline.add_option('--explain', action='store_const', const='explain',
                       dest='mode', help='Print the explain plan for the SQL and exit')
    (options, args) = cmdline.parse_args()
    
    if len(args) < 1:
        cmdline.error('No file specified')
    
    try:
        doc = Document(options, args)
        if options.mode == 'print':
            print doc.GenerateSQL()
        elif options.mode == 'explain':
            print doc.ExplainSQL()
        else:
            print doc.ExecuteSQL()
    except IOError, e:
        sys.stderr.write(str(e) + '\n')
        sys.exit(1)
    except ParseError, e:
        sys.stderr.write(str(e) + '\n')
        sys.exit(1)
    except Exception, e:
        raise
