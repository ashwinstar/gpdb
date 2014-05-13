#!/usr/bin/env python
#
# Copyright (c) Pivotal Inc 2014. All Rights Reserved.

"""
Internal Use Function.
"""

from gppylib.mainUtils import simple_main, addStandardLoggingAndHelpOptions, addMasterDirectoryOptionForSingleClusterProgram

import base64
import os
import pickle

from gppylib import gplog
from gppylib.operations.persistent_rebuild import BackupPersistentTableFiles, ValidatePersistentBackup, DEFAULT_BATCH_SIZE
from gppylib.gpparseopts import OptParser, OptChecker, OptionGroup

logger = gplog.get_default_logger()

class PersistentBackupRestore:
    def __init__(self, options, args):
        self.backup = options.backup
        self.restore = options.restore
        self.validate = options.validate
        self.timestamp = options.timestamp
        self.batch_size = options.batch_size
        self.perdbpt = options.perdbpt
        self.globalpt = options.globalpt

    def run(self):
        if not self.perdbpt or not self.globalpt:
            raise Exception('Please specify perdbpt filenames and globalpt filenames')

        if not self.batch_size:
            self.batch_size = DEFAULT_BATCH_SIZE

        if not self.timestamp:
            raise Exception('Please specify --timestamp option')

        if (self.backup and self.restore) or (self.validate and self.backup) or (self.restore and self.validate):
            raise Exception('Please specify either --backup or --restore or --validate')

        if self.backup:
            dbid_info = pickle.loads(base64.urlsafe_b64decode(self.backup))
            perdb_pt_filenames = pickle.loads(base64.urlsafe_b64decode(self.perdbpt))
            global_pt_filenames = pickle.loads(base64.urlsafe_b64decode(self.globalpt))
            BackupPersistentTableFiles(dbid_info, self.timestamp, perdb_pt_filenames, global_pt_filenames, self.batch_size).backup()
        elif self.restore:
            dbid_info = pickle.loads(base64.urlsafe_b64decode(self.restore))
            perdb_pt_filenames = pickle.loads(base64.urlsafe_b64decode(self.perdbpt))
            global_pt_filenames = pickle.loads(base64.urlsafe_b64decode(self.globalpt))
            BackupPersistentTableFiles(dbid_info, self.timestamp, perdb_pt_filenames, global_pt_filenames, self.batch_size).restore()
        elif self.validate:
            dbid_info = pickle.loads(base64.urlsafe_b64decode(self.validate))
            ValidatePersistentBackup(dbid_info, self.timestamp, self.batch_size).validate()

        return 0

    def cleanup(self):
        pass

def create_parser():
    parser = OptParser(option_class=OptChecker, 
                       version='%prog version $Revision: #1 $',
                       description='Persistent tables backp and restore')

    addStandardLoggingAndHelpOptions(parser, includeNonInteractiveOption=True)

    addTo = OptionGroup(parser, 'Connection opts')
    parser.add_option_group(addTo)
    addMasterDirectoryOptionForSingleClusterProgram(addTo)

    addTo = OptionGroup(parser, 'Persistent tables backup and restore options')
    addTo.add_option('--backup', metavar="<pickled dbid info>", type="string",
                     help="A list of dbid info where backups need to be done in pickled format")
    addTo.add_option('--restore', metavar="<pickled dbid info>", type="string",
                     help="A list of dbid info where restore needs to be done in pickled format")
    addTo.add_option('--validate', metavar="<pickled dbid info>", type="string",
                     help="A list of dbid info where validation needs to be done in pickled format")
    addTo.add_option('--timestamp', metavar="<timestamp of backup>", type="string",
                     help="A timestamp for the backup that needs to be validated") 
    addTo.add_option('--batch-size', metavar="<batch size for the worker pool>", type="int",
                      help="Batch size for parallelism in worker pool")
    addTo.add_option('--perdbpt', metavar="<per database pt filename>", type="string",  
                      help="Filenames for per database persistent files")
    addTo.add_option('--globalpt', metavar="<global pt filenames>", type="string",
                      help="Filenames for global persistent files")

    parser.setHelp([
    """
    This tool is used to backup persistent table files.
    """
    ])

    return parser

#------------------------------------------------------------------------- 
if __name__ == '__main__':
    mainOptions = { 'setNonuserOnToolLogger':True, 'suppressStartupLogMessage': True}
    simple_main(create_parser, PersistentBackupRestore, mainOptions )
