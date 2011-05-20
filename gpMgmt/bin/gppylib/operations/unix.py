import os
import fnmatch
import pickle
import shutil

from gppylib.commands.base import Command, REMOTE
from gppylib.operations import Operation
from gppylib.operations.utils import RemoteOperation

class RawRemoteOperation(Operation):
    """
    As general purpose helpers, the Operations herein may not necessarily be invoked on remote hosts that use gppylib.
    Therefore, the internal pickling mechanism of RemoteOperation will not work. A more raw mechanism is needed.
    """
    def __init__(self, cmd_str, host):
        self.cmd_str = cmd_str
        self.host = host
    def execute(self):
        cmd = Command(name = self.__class__.__name__,
                      cmdStr = self.cmd_str,
                      ctxt = REMOTE,
                      remoteHost = self.host)
        cmd.run(validateAfter=True)
        # TODO! If exception is raised remotely, there's no stdout, thereby causing a pickling error.
        return pickle.loads(cmd.get_results().stdout)
    def __str__(self): 
        return "Raw(%s, %s)" % (self.cmd_str, self.host)

class ListRemoteFiles(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(ListRemoteFiles, self).__init__(""" python -c "import os, pickle; print pickle.dumps(os.listdir('%s'))" """ % path, host)
    def __str__(self):
        return "ListRemoteFiles(%s, %s)" % (self.path, self.host)

class ListFiles(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return os.listdir(self.path)
    def __str__(self):
        return "ListFiles(%s)" % self.path

class ListRemoteFilesByPattern(RawRemoteOperation):
    def __init__(self, path, pattern, host):
        self.path, self.pattern, self.host = path, pattern, host
        super(ListRemoteFilesByPattern, self).__init__(""" python -c "import os, fnmatch, pickle; print pickle.dumps(fnmatch.filter(os.listdir('%s'), '%s'))" """ % (path, pattern), host)
    def __str__(self):
        return "ListRemoteFilesByPattern(%s, %s, %s)" % (self.path, self.pattern, self.host)

class ListFilesByPattern(Operation):
    def __init__(self, path, pattern):
        self.path, self.pattern = path, pattern
    def execute(self):
        return fnmatch.filter(os.listdir(self.path), self.pattern)
    def __str__(self):
        return "ListFilesByPattern(%s, %s)" % (self.path, self.pattern)

class CheckRemoteDir(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(CheckRemoteDir, self).__init__(""" python -c "import os, pickle; print pickle.dumps(os.path.isdir('%s'))" """ % path, host)
    def __str__(self):
        return "CheckRemoteDir(%s, %s)" % (self.path, self.host)

class CheckDir(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return os.path.isdir(self.path)
    def __str__(self):
        return "CheckDir(%s)" % self.path

class MakeRemoteDir(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(MakeRemoteDir, self).__init__(""" python -c "import os, pickle; print pickle.dumps(os.makedirs('%s'))" """ % path, host)
    def __str__(self):
        return "MakeRemoteDir(%s, %s)" % (self.path, self.host)

class MakeDir(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return os.makedirs(self.path)
    def __str__(self):
        return "MakeDir(%s)" % self.path

class CheckRemoteFile(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(CheckRemoteFile, self).__init__(""" python -c "import os, pickle; print pickle.dumps(os.path.isfile('%s'))" """ % path, host)
    def __str__(self):
        return "CheckRemoteFile(%s, %s)" % (self.path, self.host)

class CheckFile(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return os.path.isfile(self.path)
    def __str__(self):
        return "CheckFile(%s)" % self.path

class RemoveRemoteFile(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(RemoveRemoteFile, self).__init__(""" python -c "import os, pickle; print pickle.dumps(os.remove('%s'))" """ % path, host)
    def __str__(self):
        return "RemoveRemoteFile(%s, %s)" % (self.path, self.host)

class RemoveFile(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return os.remove(self.path)
    def __str__(self):
        return "RemoveFile(%s)" % self.path

class RemoveRemoteTree(RawRemoteOperation):
    def __init__(self, path, host):
        self.path, self.host = path, host
        super(RemoveRemoteTree, self).__init__(""" python -c "import shutil, pickle; print pickle.dumps(shutil.rmtree('%s'))" """ % path, host)
    def __str__(self):
        return "RemoveRemoteTree(%s, %s)" % (self.path, self.host)

class RemoveTree(Operation):
    def __init__(self, path):
        self.path = path
    def execute(self):
        return shutil.rmtree(self.path)
