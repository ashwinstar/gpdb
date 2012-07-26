#-----------------------------------------------------------------
# Generates GPDB specific mock files for cmockery.
#
# 
# based on pycarser: func_defs.py example file (Copyright (C) 2008-2011, Eli Bendersky, BSD)
#-----------------------------------------------------------------
from subprocess import Popen, PIPE
import sys
import os
from pycparser import c_parser, c_ast, parse_file
from pycparserext.ext_c_parser import GnuCParser, FuncDeclExt
from pycparserext.ext_c_generator import GnuCGenerator
from optparse import OptionParser
import logging
import subprocess
import shutil
import hashlib
import errno
import json
import copy

# These header files are not included in the generation process
header_include_black_list = set([
    "io.h", # Windows only
    "ddp_api.h", #DD Boost
    "sendalert_common.h",
    "emcconnect/api.h",
    "guc-file.c",
    "winsock2.h",
    "ws2tcpip.h",
    "sha2.h",
    "shlwapi.h",
    "sys/atomic_op.h",
    "atomic.h",
    "sys/select.h",
    "rusagestub.h",
    'procfs.h',
    "dlfcn.h"
])

def is_pointer_type(arg_node):
    """ checks is the argument node related to a pointer type """
    return type(arg_node.type) == c_ast.PtrDecl

def is_array_type(arg_node):
    """ checks is the argument node related to a array type """
    return type(arg_node.type) == c_ast.ArrayDecl

def generate_return_default(return_type):
    return "return %smock();" % (return_type)

def generate_return_XLogRecPtr(return_type):
    return """uint64_t mock__tmp_r = (uint64_t)mock();
XLogRecPtr mock__tmp_r2;
memcpy(&mock__tmp_r2, &mock__tmp_r, sizeof(XLogRecPtr));
return mock__tmp_r2;
    """
    
def generate_return_ignore(return_type):
    return """mock();
%s mock_tmp;
memset(&mock_tmp, 0, sizeof(mock_tmp));
return mock_tmp;
    """ % (return_type[1:-1])

def generate_check_default(func_decl_node, arg_node):
    """ This is the default way to generate a check_expected call for a parameter
    """
    parameter_name =  arg_node.name
        
    return [parameter_name]

def generate_check_Interval(func_decl_node, arg_node):
    """ Interval needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name         
    return ["%s.time" % (parameter_name), "%s.day" % (parameter_name), "%s.month" % (parameter_name)]

def generate_check_DbDirNode(func_decl_node, arg_node):
    """ DbDirNode needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.tablespace" % (parameter_name), "%s.database" % (parameter_name)]
       
def generate_check_XLogRecPtr(func_decl_node, arg_node):
    """ XLogRecPtr needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.xlogid" % (parameter_name), "%s.xrecoff" % (parameter_name)]

def generate_check_RelFileNode(func_decl_node, arg_node):
    """ RelFileNode needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.spcNode" % (parameter_name), "%s.dbNode" % (parameter_name), "%s.relNode" % (parameter_name)]

def generate_check_ItemPointerData(func_decl_node, arg_node):
    """ ItemPointerData needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.ip_blkid.bi_hi" % (parameter_name), "%s.ip_blkid.bi_lo" % (parameter_name), "%s.ip_posid" % (parameter_name)]

def generate_check_PGSemaphoreData(func_decl_node, arg_node):
    """ PGSemaphoreData needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.semId" % (parameter_name), "%s.semNum" % (parameter_name)]

def generate_check_CdbPathLocus(func_decl_node, arg_node):
    """ CdbPathLocus needs special treatment, because it is a structure that is passed by
        value.
    """
    parameter_name =  arg_node.name
    return ["%s.locustype" % (parameter_name), "%s.partkey" % (parameter_name)]

def generate_check_ignore(func_decl_node, arg_node):
    # Output not check
    return []

def has_return_type_name(return_decl):
    return "names" in dir(return_decl.type) 

def return_type_is_void(return_decl):
    return return_decl.type.names[0] == "void"

def get_return_type_name(return_decl):
    return return_decl.type.names[0]

def get_default_mock_info(func_decl_node):
    mock_function_info = {"parameter": [], 
                          "header filename": func_decl_node.coord.file}    
    return mock_function_info

def generate_funcion_body_default(func_decl):
    """ Default way to generate the body of a mocked function
    """
    def getCheckGeneratorForType(arg_node):
        """ Retrieves to correct check generator function for a given argument node
            This is used for a special handling of specific types, e.g. structures that
            are not passed via a pointer
        """
        if is_pointer_type(arg_node) or is_array_type(arg_node):
            generater_function = generate_check_default
        else:
            generator_helper = GnuCGenerator()
            parameter_type = generator_helper.visit(arg_node.type.type)
            #parameter_type = " ".join(arg_node.type.type.names)
            generater_function = type_special_case.get(parameter_type, generate_check_default)
        return generater_function

    def getReturnGeneratorForType(return_type):
        """ Retrieves to correct return generator function for a given return type
            This is used for a special handling of specific types, e.g. structures that
            are not passed via a pointer
        """
        if return_type.find("*") >= 0:
            # Pointer type
            return generate_return_default
        raw_return_type = return_type[1:-1]
        generater_function = return_type_special_case.get(raw_return_type, generate_return_default)
        return generater_function
    
    mock_function_info = get_default_mock_info(func_decl)
    check_lines = []
    assign_lines = []
    if func_decl.type.args:   
        for arg_child in func_decl.type.args.children():
            arg_node = arg_child[1]
            if type(arg_node) == c_ast.EllipsisParam:
                # variable argument (...) parameter
                pass  
            elif not arg_node.name:
                # Check for void
                if 'declname' in dir(arg_node.type) and not arg_node.type.declname:
                    pass
                else:
                    # It is not possible to generate something when there is no name given
                    # BTW: It is also not a good code style
                    return False
            elif type(arg_node) == c_ast.Decl:
                # Get the generator function depending on the parameter type
                generater_function = getCheckGeneratorForType(arg_node)
                new_check_parameters = generater_function(func_decl, arg_node)
                
                for new_check_parameter in new_check_parameters:
                    check_lines.append("check_expected(%s);" % new_check_parameter)
                
                if is_pointer_type(arg_node) and "const" not in arg_node.quals:
                    assign_line = "optional_assignment(%s);" % (arg_node.name)
                    assign_lines.append(assign_line)
                
                mock_function_info["parameter"].append(arg_node.name)
            else:
                print "Other Parameter"
                print arg_node.type.declname
                print dir(arg_node.type)
                arg_node.show() 
                sys.exit(1)
    else:
        #Come on guys, in C this means that the function takes a unspecified number
        #of arguments. C != C++.
        pass

    return_decl = func_decl.type.type
    return_type = None
    if type(return_decl) == c_ast.TypeDecl:
        if has_return_type_name(return_decl) and not return_type_is_void(return_decl):
            return_type = "(%s)" % (get_return_type_name(return_decl))
            mock_function_info["return"] = get_return_type_name(return_decl)
        elif has_return_type_name(return_decl) and return_type_is_void(return_decl):
            mock_function_info["return"] = get_return_type_name(return_decl)
    elif type(return_decl) == c_ast.PtrDecl:
        ptr_decl = return_decl
        return_decl = ptr_decl.type
        if has_return_type_name(return_decl):
            return_type = "(%s *)" % (get_return_type_name(return_decl))
            mock_function_info["return"]  = "%s*" % get_return_type_name(return_decl)
    else:
        return_decl.show()
    
    mock_line = "mock();"
    if return_type:
        return_generater_function = getReturnGeneratorForType(return_type)
        mock_line = return_generater_function(return_type)  
    mock_function_body = """
    %s
    %s
    %s
""" % ("\n".join(check_lines),
        "\n".join(assign_lines),
        mock_line)

    return (mock_function_body, mock_function_info)


def generate_function_body_MemoryContextAllocZeroImpl(func_decl_node):
    return ("""
    void* p = malloc(size);
    memset(p, 0, size);
    return p;
    """, get_default_mock_info(func_decl_node))
    
def generate_function_body_MemoryContextAllocImpl(func_decl_node):
    return ("""
    void* p = malloc(size);
    return p;
    """, get_default_mock_info(func_decl_node))    

def generate_function_body_MemoryContextFreeImpl(func_decl_node):
    return ("""
    free(pointer);
    """ , get_default_mock_info(func_decl_node))

def generate_function_body_MemoryContextStrdup(func_decl_node):
    return ("""
    return strdup(string);
    """, get_default_mock_info(func_decl_node)) 

def generate_function_body_MemoryContextReallocImpl(func_decl_node):
    return ("""
    return realloc(pointer, size);
    """, get_default_mock_info(func_decl_node)) 

def generate_function_body_MemoryContextAllocZeroAlignedImpl(func_decl_node):
    return ("""
    void* p = malloc(size);
    return p;
    """, get_default_mock_info(func_decl_node)) 

def generate_function_body_MemoryContextAllocZeroAlignedImpl(func_decl_node):
    return ("""
    void* p = malloc(size);
    return p;
    """, get_default_mock_info(func_decl_node)) 

def generate_function_body_namestrcpy(func_decl_node):
    return ("""
    if (!name || !str)
        return -1;
    StrNCpy(NameStr(*name), str, NAMEDATALEN);
    return 0;
    """, get_default_mock_info(func_decl_node))
    
# Special case check line generators
# especially needed for struct parameters
type_special_case = {
    "XLogRecPtr": generate_check_XLogRecPtr,
    "RelFileNode": generate_check_RelFileNode,
    "ItemPointerData": generate_check_ItemPointerData,
    "PGSemaphoreData": generate_check_PGSemaphoreData,
    "CdbPathLocus": generate_check_CdbPathLocus,
    "FileRepIdentifier_u": generate_check_ignore,
    "FileRepOperationDescription_u": generate_check_ignore,
    "FileRepRelFileNodeInfo_s": generate_check_ignore,
    "instr_time": generate_check_ignore,
    "DbDirNode": generate_check_DbDirNode,
    "VariableStatData": generate_check_ignore,
    "Interval": generate_check_Interval
}

return_type_special_case = {
    "XLogRecPtr": generate_return_XLogRecPtr,
    "FileRepIdentifier_u": generate_return_ignore,
    "CdbPathLocus": generate_return_ignore,
    "RelFileNode": generate_return_ignore
}

function_name_special_case = {
    "MemoryContextAllocZeroImpl": generate_function_body_MemoryContextAllocZeroImpl,
    "MemoryContextAllocImpl": generate_function_body_MemoryContextAllocImpl,
    "MemoryContextFreeImpl": generate_function_body_MemoryContextFreeImpl,
    "MemoryContextStrdup": generate_function_body_MemoryContextStrdup,
    "MemoryContextReallocImpl": generate_function_body_MemoryContextReallocImpl,
    "MemoryContextAllocZeroAlignedImpl": generate_function_body_MemoryContextAllocZeroAlignedImpl,
    'namestrcpy': generate_function_body_namestrcpy
}

function_name_skip = {
    "gp_pthread_create": None,
    "parseRelOptions": None,
    "gp_connect": None,
    "SyncAgentMain": None,
    "ExceptionalCondition": ['postgres.c'],
    "gist_redo": ["gist.c"],
    "gist_desc": ["gist.c"],
    "gist_xlog_startup": ["gist.c"],
    "gist_xlog_cleanup": ["gist.c"],
    "gist_safe_restartpoint": ["gist.c"],
    "gist_form_invalid_tuple": ["gist.c"],
    "formUpdateRdata": ["gist.c"],
    "formSplitRdata": ["gist.c"],
    "formCreateRData": ["gist.c"],
    "gistxlogInsertCompletion": ["gist.c"],
    "GetSlotTableDebugInfo": ["tqual.c"],
    "BackoffBackendTick": ['postgres.c'],
    "MemoryContextError": None
}

# For these files, only functions that match these prefix are generated
header_file_prefix_filter = {
     "md.c": ["md", "RememberFsyncRequest", "ForgetRelationFsyncRequests", "ForgetDatabaseFsyncRequests"],
     "smgr.c": ["smgr", 'StorageManagerMirrorMode_Name',
                'AtSubCommit_smgr', 'AtSubAbort_smgr', 'AtEOXact_smgr', 
                'PostPrepare_smgr'],
    "name.c": ["name"]
}

header_file_special_case = {
    "aocsam.c": ["cdb/cdbaocsam.h"],
    "appendonlyam.c": ["cdb/cdbappendonlyam.h"],
    "appendonlyblockdirectory.c":  ["cdb/cdbappendonlyblockdirectory.h"],
    "fts.c": ["postmaster/fts.h"],
    "aset.c": ["utils/memutils.h"],
    "mcxt.c": ["utils/palloc.h"],
    "cdbcat.c": ["catalog/gp_policy.h", "cdb/cdbcat.h"],
    "gist.c": ["access/gist.h", "access/gist_private.h"],
    "combocid.c": ["utils/combocid.h", 'access/htup.h'],
    "relcache.c": ["utils/relcache.h", 'utils/rel.h'],
    "postgres.c": ['postgres.h', "miscadmin.h"],
    "bufmgr.c": ["storage/bufmgr.h", "storage/buf_internals.h"],
    "fmgr.c": ["fmgr.h"],
    "md.c": ['storage/smgr.h'],
    "execTuples.c": ['executor/tuptable.h'],
    "name.c": ['utils/builtins.h']
}

def execute_c_preprocessor(filename, cpp_args):
    """ taken from pycparser.py
    """
    path_list = ["cpp"]
    if isinstance(cpp_args, list):
        path_list += cpp_args
    elif cpp_args != '': 
        path_list += [cpp_args]
    path_list += [filename]
    
    try:
        # Note the use of universal_newlines to treat all newlines
        # as \n for Python's purpose
        #
        pipe = Popen(   path_list, 
                        stdout=PIPE, 
                        universal_newlines=True)
        text = pipe.communicate()[0]
        return text
    except OSError as e:
        raise RuntimeError("Unable to invoke 'cpp'.  " +
            'Make sure its path was passed correctly\n' +
            ('Original error: %s' % e))
    
class MockFunctionGenerationVisitor(c_ast.NodeVisitor):
    """ Visitor class that does the main mock function generation work
    """
    def __init__(self, options, source_filename, header_filenames, mock_info_map, mock_file):
        self.options = options
        self.source_filename = source_filename
        self.header_filenames = header_filenames
        self.mock_file = mock_file
        self.mock_info_map = mock_info_map
        self.generator_helper = GnuCGenerator()
        
    def visit_Struct(self, node):
        """ visit the structure.
            This visitor function is introduced to avoid the visiting of all nested Decl
            nodes in the struct.
            
            This helps that all visit_Decl call are for top-level variable declarations
        """
        pass
    
    def visit_Union(self, node):
        """ visit the union.
            This visitor function is introduced to avoid the visiting of all nested Decl
            nodes in the union.
            
            This helps that all visit_Decl call are for top-level variable declarations
        """
        pass
    
    def visit_Enum(self, node):
        """ visit the enum.
            This visitor function is introduced to avoid the visiting of all nested Decl
            nodes in the enum.
            
            This helps that all visit_Decl call are for top-level variable declarations
        """
        pass

    def visit_Typedef(self, node):
        """ visit the typedef.
            This visitor function is introduced to avoid the visiting of all nested Decl
            nodes in the typedef.
            
            This helps that all visit_Decl call are for top-level variable declarations
        """
        pass
            
    def check_filename_based_function_skip(self, func_decl):
        if func_decl.name in function_name_skip:
            # Now a more detailed check
            file_list = function_name_skip[func_decl.name]
            if file_list is None:
                return False
            if os.path.basename(self.source_filename) in file_list:
                return False
        pf = header_file_prefix_filter.get(os.path.basename(self.source_filename), None)
        if pf:
            for prefix in pf:
                if func_decl.name.startswith(prefix):
                    break
            else:
                # No match
                return False
        return True
            
    def process_Decl_FuncDeclExt(self, func_decl):
        if "static" in func_decl.storage:
            return
        if not self.check_filename_based_function_skip(func_decl):
            return
        
        generate_function = function_name_special_case.get(func_decl.name, 
                generate_funcion_body_default)
        (mock_function_body, mock_info) = generate_function(func_decl)
        if not mock_function_body:
            logging.info("Skipping function %s", func_decl.name)
            return
        
        if mock_info:
            # Normalize filenames
            mock_info["filename"] = os.path.relpath(self.source_filename, self.options.cdb2_src_dir)
            mock_info["header filename"] = os.path.relpath(mock_info["header filename"], self.options.cdb2_src_dir)
            self.mock_info_map["functions"][func_decl.name] = mock_info
        elif func_decl.name in self.mock_info_map:
            del self.mock_info_map["functions"][func_decl.name]
        
        if "extern" in func_decl.storage:
            func_decl.storage.remove("extern")
        mock_function = """%s {
    %s
    }
    
""" % (self.generator_helper.visit_Decl(func_decl), 
        mock_function_body)
    
        if self.mock_file:
            self.mock_file.write(mock_function)
               
    def process_Decl_Variable(self, node):
        if "extern" in node.storage:
            node.storage.remove("extern")
        line = "%s;\n" % (self.generator_helper.visit_Decl(node))
        
        mock_info = {}
        mock_info["filename"] = os.path.relpath(self.source_filename, self.options.cdb2_src_dir)
        mock_info["header filename"] = os.path.relpath(node.coord.file, self.options.cdb2_src_dir)
        self.mock_info_map["globals"][node.name] = mock_info
        
        if self.mock_file:
            self.mock_file.write(line)          
    
    def visit_Decl(self, node):
        """ Generation of copies for all globally visible global variables
        """
        
        if not node.coord:
            return  
        for header_file in self.header_filenames:
            if node.coord.file.endswith("/" + header_file):
                break
        else:
            # Not the correct header file
            return
        
        if "static" in node.storage:
            return
        if type(node.type) == FuncDeclExt:
            self.process_Decl_FuncDeclExt(node)
            return
        if type(node.type) == c_ast.Struct:
            return
        if type(node.type) == c_ast.Enum:
            return
        self.process_Decl_Variable(node)

    def visit_FuncDef(self, node):
        pass

def extract_includes(filename):
    """ Extract all (simple) includes from a source file
    """
    include_list = []

    for line in open(filename):
        if line.startswith("#include"):
            for black_list_header in header_include_black_list:
                if line.find("<%s>" % black_list_header) >= 0:
                    break
                if line.find("\"%s\"" % black_list_header) >= 0:
                    break
            else:
                include_list.append(line.strip())
    return include_list

def get_mock_filename(filename, options, deep=False):
    """ Get the filename of the mock source file for a given file
    """
    basename = os.path.basename(filename)
    basepartitions = basename.rpartition(".")
    
    if deep:
        p = os.path.dirname(filename).partition("backend")
        if p[2]:
            out_dir = os.path.join(options.out_dir, p[2][1:])
            if not os.path.exists(out_dir):
                os.makedirs(out_dir)
        else:
            out_dir = options.out_dir
    else:
        out_dir = options.out_dir
    
    return os.path.join(out_dir, "%s_mock.c" % (basepartitions[0]))

def set_perforce_edit_state(filename):
    """
        Opens a file for edit in perforce
    """
    subprocess.call("p4 open %s" % filename, shell=True)

def set_perforce_add_file(filename):
    """
        Adds a file in perforce
    """
    subprocess.call("p4 open %s" % filename, shell=True)
    
def indent_source_file(filename):
    subprocess.call("indent -bl -i4 %s" % filename, shell=True)
    bak_filename = os.path.basename(filename)  + '.BAK'
    if os.path.exists(bak_filename):
        os.remove(bak_filename)
            
def file_is_identical(filename1, filename2):
    """ Returns true iff filename1, filename2 has an identical content
        Meta data is not checked
    """
    h1 = hashlib.md5(open(filename1).read()).hexdigest()
    h2 = hashlib.md5(open(filename2).read()).hexdigest()
    return h1 == h2

def get_associated_header_file(cdb2_src_dir, c_filename):
    end_filename = os.path.basename(c_filename)
    
    # Watch for special cases/inconsistencies
    if header_file_special_case.get(end_filename):
        h = header_file_special_case.get(end_filename)
        return h
        
    dirs = os.path.dirname(os.path.relpath(c_filename, os.path.join(cdb2_src_dir, "cdb-pg/src/backend")))
    component_dir = dirs.split("/")[0]
    
    h_filename = os.path.join(component_dir, os.path.splitext(end_filename)[0] + ".h")
    
    if not os.path.exists(os.path.join(cdb2_src_dir, "cdb-pg/src/include", h_filename)):
        return
    
    return [h_filename]

def get_cpp_args(options):
    cpp_args=[
    r'-Iutils/fake_libc_include', 
    r'-Iutils/fake_cdb_include',
    r'-I%s' % (os.path.expanduser(os.path.join(options.cdb2_src_dir, "cdb-pg/src/include"))),
    r'-I%s' % (os.path.expanduser(os.path.join(options.cdb2_src_dir, "ext/osx105_x86/include/"))),
    r'-I%s' % (os.path.expanduser(os.path.join(options.cdb2_src_dir, "cdb-pg/src/interfaces/libpq"))),
    r'-I%s' % (os.path.expanduser(os.path.join(options.cdb2_src_dir, "cdb-pg/src/backend/gp_libpq_fe"))),
    r'-DCDB_MOCKING']  
    return cpp_args    

def write_tmp_source_file(source_include_list, header_filenames):
    file_contents = """
%s
%s
""" % ("\n".join(source_include_list), 
       "\n".join("#include \"%s\"" % h for h in header_filenames))

    f = open ("/tmp/cmock_gen.c", "w")
    f.write(file_contents)
    f.close()
    
    return "/tmp/cmock_gen.c"  
                       

def write_mock_file_header(options, source_include_list, mock_filename, header_filenames):
    if not options.try_run:
        mock_file = open(mock_filename + ".tmp", "w")
        mock_file.write("""/* 
 *
 * Auto-generated Mocking Source
 *
 * Source: %s
 *
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

%s
 
""" % (', '.join(header_filenames), "\n".join(source_include_list)))
    else:
        mock_file = None
    return mock_file

def process_source_file(filename, mock_info_map, options):    
    print filename
    
    cpp_args = get_cpp_args(options)    
    
    header_filenames = get_associated_header_file(options.cdb2_src_dir, filename)
    if not header_filenames:
        logging.info("Skipping %s (Headerless File)", filename)
        return
           
    source_include_list = extract_includes(filename)
    tmp_filename = write_tmp_source_file(source_include_list, header_filenames)
    
    try:
        if options.output_cpp:
            print execute_c_preprocessor(tmp_filename, cpp_args)
        
        mock_filename = get_mock_filename(filename, options)
        ast = parse_file(tmp_filename, use_cpp=True, cpp_args=cpp_args, parser=GnuCParser())      
        
        if options.output_ast:
            ast.show()

        mock_file = write_mock_file_header(options, source_include_list, mock_filename, header_filenames)
        
        v = MockFunctionGenerationVisitor(options, filename, header_filenames, mock_info_map, mock_file)
        v.visit(ast)
        
        if mock_file:
            mock_file.flush()
            mock_file.close()
            
            mock_tmp_filename = mock_filename + ".tmp"
            indent_source_file(mock_tmp_filename)
            mock_file_existed = os.path.exists(mock_filename)
            if not mock_file_existed or not file_is_identical(mock_filename, mock_tmp_filename):
                try:
                    shutil.copy(mock_tmp_filename, mock_filename)
                except IOError as e:
                    if (e.errno == 13 or e.errno == 1) and options.with_p4:
                        set_perforce_edit_state(mock_filename)
                        shutil.copy(mock_tmp_filename, mock_filename)
                    else:
                        raise
            
            if not mock_file_existed and options.with_p4:
                set_perforce_add_file(mock_filename)
            # Cleanup
            if os.path.exists(mock_tmp_filename):
                os.unlink(mock_tmp_filename)
            
            #os.rename(mock_filename + ".tmp", mock_filename)
    except Exception as e:
        logging.exception("Failed to generate mock for file %s", filename)

def load_mock_file_list(filename):
    source_file_list = set()
    
    for filename in open(filename):
        filename = filename.strip()
        
        # Remove comments
        filename = filename.partition('#')[0]
        filename = filename.strip()
        if len(filename) > 0:
            source_file_list.add(filename)
    return source_file_list

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)

    parser = OptionParser()
    parser.add_option("--out_dir",
                  dest="out_dir", default="../",
                  help="mock source output directory")
    parser.add_option("--cdb2_src_dir",
                  dest="cdb2_src_dir", default="../../../../../..",
                  help="cdb2 source input directory")
    parser.add_option("--try_run", 
                  action="store_true",
                  dest="try_run", default=False,
                  help="try run")
    parser.add_option("--output_cpp", 
                  action="store_true",
                  dest="output_cpp", default=False,
                  help="output_cpp")
    parser.add_option("--output_ast", 
                  action="store_true",
                  dest="output_ast", default=False,
                  help="output_ast")       
    parser.add_option("--with_p4", 
                  action="store_true",
                  dest="with_p4", default=False,
                  help="Calls p4 open on all changes mock files") 
    parser.add_option("--ignore_mock_source_file", 
                  action="store_true",
                  dest="ignore_mock_source_file", default=False,
                  help="ignore_mock_source_file")       
    parser.add_option("--ignore_blacklist", 
                  action="store_true",
                  dest="ignore_blacklist", default=False,
                  help="ignore_blacklist")     
    (options, args) = parser.parse_args()
    
    source_mock_file_list = load_mock_file_list("mock_source")
    source_blacklist = load_mock_file_list("mock_blacklist")
    
    cdb2_src_dir = os.path.abspath(os.path.expanduser(options.cdb2_src_dir))
    if not os.path.exists(os.path.join(cdb2_src_dir, "cdb-pg")):
        logging.error("Illegal --cdb2_src_dir")
        sys.exit(1)       
        
    if len(args) == 0:
        # Use a default if no argument is given
        args = [os.path.abspath(
            os.path.expanduser(
                os.path.join(options.cdb2_src_dir,
                    "cdb-pg/src/backend")))] 
        
    # The mock info json file contains basic information about the mock source files, but in an easy to parse format
    # The mock source files and the json file should be kept consistent. A current problem are manual generated files and
    # functions that are deleted from a source file.
    mock_info_json_filename = os.path.join(options.out_dir, "mock_info.json")
    if os.path.exists(mock_info_json_filename):
        mock_info_map = json.load(open(mock_info_json_filename))
    else:
        # mock info file doesn't exists
        mock_info_map = {"functions": {}, "globals": {}} 
    original_mock_info_map = copy.deepcopy(mock_info_map)
    
    for arg in args:
        filename = arg

        if filename.endswith('.c'):
            if not os.path.exists(filename):
                logging.info("Skipping %s (File Not Found)", os.path.basename(filename))
            elif not options.ignore_blacklist and os.path.basename(filename) in source_blacklist:
                logging.info("Skipping %s (Known Issue Or Known Manual File)", os.path.basename(filename))
            else:    
                process_source_file(filename, mock_info_map, options)
        elif os.path.isdir(filename):
            for root, dirs, files in os.walk(filename):
                for f in files:
                    if not options.ignore_mock_source_file and not f in source_mock_file_list:
                        continue
                    if not options.ignore_blacklist and f in source_blacklist:
                        logging.info("Skipping %s (Known Issue Or Known Manual File)", f)
                        continue    
                    if not f.endswith(".c"):
                        continue
                    process_source_file(os.path.join(root, f), mock_info_map, options)
        else:
            logging.error("Skipping %s", arg)
            
    # Write out the updated information
    if mock_info_map != original_mock_info_map:
        if os.path.exists(mock_info_json_filename):
            set_perforce_edit_state(mock_info_json_filename)
        json.dump(mock_info_map, open(mock_info_json_filename, "w"), sort_keys=True, indent=4)
