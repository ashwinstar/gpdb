#!/usr/bin/env python

# Copyright (c) 2014 Pivotal Software, Inc. All Rights Reserved
#
# This software contains the intellectual property of Pivotal Software, Inc.
# or is licensed to Pivotal Software, Inc. from third parties. Use of this
# software and the intellectual property contained therein is expressly
# limited to the terms and conditions of the License Agreement under which
# it is provided by or on behalf of Pivotal Software, Inc.

import base64
import os
import pickle
import platform
import re
import sys

from collections import defaultdict
from datetime import datetime
from gppylib import gplog, gpversion
from gppylib.commands.base import Command, ExecutionError, WorkerPool, REMOTE
from gppylib.commands.gp import GpStop, GpStart, GpVersion
from gppylib.commands.unix import findCmdInPath
from gppylib.db import dbconn
from gppylib.gparray import GpArray
from gppylib.operations import Operation
from gppylib.operations.utils import RemoteOperation, ParallelOperation
from gppylib.userinput import ask_yesno

logger = gplog.get_default_logger()

DEFAULT_BATCH_SIZE = 16
SYSTEM_FSOID = 3052
DEFAULT_DATABASE = 'postgres'
TIMESTAMP = datetime.now().strftime("%Y%m%d%H%M%S")
GLOBAL_PERSISTENT_FILES = []
PER_DATABASE_PERSISTENT_FILES = defaultdict(list)  #The key for this dictionary is the dboid and
                                                   #the value is a list containing the list of 
                                                   #relfilenode ids of the perdb pt files (5094, 5095)

class ValidateContentID:
    '''
    Parses the content_id or the contentid_file passed in by the user and
    validates the content ids.
    '''
    def __init__(self, content_id, contentid_file, gparray):
        self.content_id = content_id
        self.contentid_file = contentid_file
        self.gparray = gparray

    def _parse_content_id(self):
        '''
        Parse the content id passed in by the user and
        return it as a list of integers
        '''
        try:
            self.content_id = self.content_id.strip().split(',')
            self.content_id = map(int, self.content_id)
        except Exception as e:
            logger.error('Please correct the input data and try again')
            raise Exception('Some content ids are not integers: %s.' % self.content_id)

        return self.content_id

    def _validate_contentid_file(self):
        '''
        Validate the file for invalid integers
        Skips blank lines 
        '''
        if not os.path.isfile(self.contentid_file):
            raise Exception('Unable to find contentid file "%s"' % self.contentid_file)

        self.content_id = []

        with open(self.contentid_file) as fp:
            for line in fp:
                line = line.strip()
                if line:
                    try:
                        line = int(line)
                    except Exception as e:
                        logger.error('File should contain integer content ids one per line. Please correct the input data and try again')
                        raise Exception('Found non integer content id "%s" in contentid file "%s"' % (line, self.contentid_file))
                    self.content_id.append(line)

        if not self.content_id:
            raise Exception('Please make sure there is atleast one integer content ID in the file')

        return self.content_id

    def _validate_content_id(self):
        """
        Validates that the content ids are valid contents by checking 
        gp_segment_configuration and that atleast the primary is up.
        Returns error if content id is not present in gp_segment_configuration
        or both primary and mirror are down
        """

        """Collect a list of all content ids that are in the gp_segment_configuration"""
        valid_content_ids = set([seg.getSegmentContentId() for seg in self.gparray.getDbList()])
        invalid_content_ids = set()

        for c in self.content_id:
            if c not in valid_content_ids:
                invalid_content_ids.add(c)

        if len(invalid_content_ids) > 0:
            raise Exception('The following content ids are not present in gp_segment_configuration: %s' % ', '.join(map(str, invalid_content_ids)))

        """
            Collect a list of all segments where either primary or mirror is up. Whichever segment is up
            will be acting as primary. If we don't find even one segment that is up for a particular
            content, then it mean both primary and mirror are down. Hence down_content_ids will only 
            be populated if both the primary and mirror are down and not if only one of them is down.
        """
        up_content_ids = set([seg.getSegmentContentId() for seg in self.gparray.getDbList() if seg.getSegmentStatus() != 'd'])
        down_content_ids = set()
        resync_content_ids = set()

        for seg in self.gparray.getDbList():
            status = seg.getSegmentStatus()
            c = seg.getSegmentContentId()
            if status == 'u':
                up_content_ids.add(c)
            elif status == 'r':
                resync_content_ids.add(c)

        for c in self.content_id:
            if c not in up_content_ids:
                down_content_ids.add(c)

        if len(down_content_ids) > 0:
            raise Exception('The following content ids are down: %s' % ', '.join(map(str, down_content_ids)))

        if len(resync_content_ids) > 0:
            logger.error('Rebuild of persistent tables is not supported with segments in resync mode')
            raise Exception('The following content ids are in resync mode: %s' % ', '.join(map(str, resync_content_ids)))

        return self.content_id

    def validate(self):
        """
        We make sure that the user does not pass in both content_id and
        contentid_file in gppersistentrebuild and that alteast one of
        them is definitely passed in. All the content-ids are populated
        in self.content_id and that is validated against gp_segment_configuration
        """
        if self.content_id:
            self._parse_content_id()
        elif self.contentid_file:
            self._validate_contentid_file()
        return self._validate_content_id()

class DbIdInfo:
    """
    Stores all the information regarding a single dbid
    """
    def __init__(self, content, dbid, port, hostname, filespace_dirs, fs_to_ts_map, ts_to_dboid_map):
        self.content = content
        self.dbid = dbid
        self.port = port
        self.hostname = hostname
        self.filespace_dirs = filespace_dirs
        self.fs_to_ts_map = fs_to_ts_map
        self.ts_to_dboid_map = ts_to_dboid_map

    def __eq__(self, other):
        return  vars(self) == vars(other)

    def __str__(self):
        return '%s:%s:%s:%s:%s:%s:%s' % (self.content, self.dbid, self.port, self.hostname, self.filespace_dirs, 
                                         self.fs_to_ts_map, self.ts_to_dboid_map)

class GetDbIdInfo:
    """
    Query the catalog tables to get information about
    filespaces/tablespaces and persistent table files
    for each dbid
    """
    def __init__(self, gparray, content_id):
        self.gparray = gparray
        self.content_id = content_id
   
    def _get_filespace_to_tablespace_map(self, segdb):
        '''
        Get a map of filespace oids to tablespace oids for a given segdb
        The key is a single integer representing the oid of a filespace
        The value is a list of oids which represent the oids of 
        tablespaces
        '''
        fs_to_ts_map = {}
        fs_oids = segdb.getSegmentFilespaces().keys() 
        FILESPACE_TO_TABLESPACE_MAP_QUERY = """SELECT spcfsoid, string_agg(oid, ' ')
                                               FROM pg_tablespace
                                               WHERE spcfsoid IN (%s) GROUP BY spcfsoid""" % ', '.join(map(str, fs_oids))  
        with dbconn.connect(dbconn.DbURL(dbname=DEFAULT_DATABASE)) as conn:
            res = dbconn.execSQL(conn, FILESPACE_TO_TABLESPACE_MAP_QUERY)
            for r in res:
                fs_to_ts_map[r[0]] = [int(x) for x in r[1].split()]
        return fs_to_ts_map

    def _get_tablespace_to_dboid_map(self, ts_oids):
        '''
        Get a map of tablespaces oids to database oids for a given tablespace
        The key is a single integer representing the oid of a tbalespace
        The value is a list of oids which represent the oids of databases
        '''
        ts_to_dboid_map = {}
        TABLESPACE_TO_DBOID_MAP_QUERY = """SELECT dattablespace, string_agg(oid, ' ')
                                           FROM pg_database
                                           WHERE dattablespace IN (%s) GROUP BY dattablespace""" % ', '.join(map(str, ts_oids))  
        with dbconn.connect(dbconn.DbURL(dbname=DEFAULT_DATABASE)) as conn:
            res = dbconn.execSQL(conn, TABLESPACE_TO_DBOID_MAP_QUERY)
            for r in res:
                ts_to_dboid_map[r[0]] = [int(x) for x in r[1].split()]
        return ts_to_dboid_map

    def get_info(self):
        '''
        This method gets the information for all the segdbs where we want to rebuild the
        persistent tables. 
        It returns a list of DbIdInfo objects
        '''
        dbid_info = []
        
        for seg in self.gparray.getDbList():
            if seg.getSegmentContentId() in self.content_id and seg.getSegmentRole() == 'p' and seg.getSegmentStatus() != 'd':
                fs_to_ts_map = self._get_filespace_to_tablespace_map(seg)
                ts_oids = []
                for fsoid, ts in fs_to_ts_map.items():
                    ts_oids += ts 
                ts_to_dboid_map = self._get_tablespace_to_dboid_map(ts_oids)
                di = DbIdInfo(content=seg.getSegmentContentId(), dbid=seg.getSegmentDbId(), port=seg.getSegmentPort(),
                              hostname=seg.getSegmentHostName(), filespace_dirs=seg.getSegmentFilespaces(),
                              fs_to_ts_map=fs_to_ts_map, ts_to_dboid_map=ts_to_dboid_map)
                dbid_info.append(di)

        return dbid_info

class ValidateMD5Sum:
    """Checks the md5 sum for a list of files"""
    def __init__(self, pool, batch_size=DEFAULT_BATCH_SIZE):
        self.batch_size = batch_size
        self.md5_prog = None
        self.md5_results_pat = None
        self.pool = pool 

    def _get_md5_prog(self):
        """Get the appropriate md5 program for the platform"""
        md5_prog = ''
        operating_sys = platform.system()
        if operating_sys == 'Darwin':
            md5_prog = 'md5'
        elif operating_sys == 'Linux':
            md5_prog = 'md5sum'
        else:
            raise Exception('Cannot determine the md5 program since %s platform is not supported' % operating_sys)
        return md5_prog

    def _get_md5_results_pat(self):
        """
        We want to parse the results of the md5progs in order to extract the filename
        and its correspoding md5sum.
        On OSX, the md5 program will return output in the following format
        MD5 (<filename>) = <md5_hash> 
        On Linux, the md5 program will return output in the following format
        <md5_hash> <filename>
        Hence this returns an re.pattern object so that we can extract the required
        information
        """
        operating_sys = platform.system()
        if operating_sys == 'Darwin':
            md5_pat = re.compile('MD5 \((.*)\) = (.*)')
        elif operating_sys == 'Linux':
            md5_pat = re.compile('(.*) (.*)')
        else:
            raise Exception('Cannot determine the pattern for results of md5 program since %s platform is not supported' % operating_sys)
        return md5_pat

    def init(self):
        """
        Initialize the class with the md5 program and the pattern
        based on the platform
        Ideally this should be called once per run of the program 
        in order to be efficient. It is the callers reponsibilty
        to ensure that.
        """
        self.md5_prog = self._get_md5_prog()
        self.md5_results_pat = self._get_md5_results_pat()

    def _process_md5_results(self):
        """
        Returns a dictionary with the key as the filename
        and the value as the md5 hash value
        If there was any error, it raises an Exception
        """
        md5s = {} 
        for item in self.pool.getCompletedItems():
            result = item.get_results()
            if not result.wasSuccessful():
                raise Exception('Unable to calculate md5sum for: %s' % (result.stderr.strip()))

            md5_results = result.stdout.strip().split('\n')
            for md5_result in md5_results:
                mat = self.md5_results_pat.match(md5_result.strip())
                if mat:
                    if platform.system() == 'Linux':
                        f, md5 = mat.group(2), mat.group(1)
                    else:
                        f, md5 = mat.group(1), mat.group(2)
                    md5s[os.path.basename(f.strip())] = md5
        return md5s 

    def validate(self, src_files):
        """Run the md5 program and calculate the md5sum for the src_files"""
        for f in src_files:
            cmd = Command('calculate md5sum for file', cmdStr='%s %s' % (self.md5_prog, f))
            self.pool.addCommand(cmd)
        self.pool.join()
        return self._process_md5_results()

class BackupPersistentTableFiles:
    """
    Backup all the global and the per database persistent table files
    """
    def __init__(self, dbid_info, timestamp, perdb_pt_filenames, global_pt_filenames, batch_size=DEFAULT_BATCH_SIZE):
        self.dbid_info = dbid_info
        self.timestamp = timestamp
        self.batch_size = batch_size
        self.md5_validator = None
        self.pool = None
        self.GLOBAL_PERSISTENT_FILES = global_pt_filenames 
        self.PER_DATABASE_PERSISTENT_FILES = perdb_pt_filenames 

    def _cleanup_pool(self):
        if self.pool:
            self.pool.haltWork()
            self.pool.joinWorkers()
            self.pool = None

    def _copy_pt_files(self, src_files, dest_files, host, restore=False):
        """
        This actually does the copy of the files from src directory to backup directory 
        In case of backup, the destination folder might not exist. Hence we create it.
        While restoring it, we always restore to datadirectory and it should be present, 
        hence we do not bother to create it.
        """
        src_md5 = self.md5_validator.validate(src_files)

        if (not restore) and len(dest_files) > 0:
            cp_dir = os.path.dirname(dest_files[0])
            os.makedirs(os.path.dirname(dest_files[0]))
        elif len(dest_files) > 0:
            cp_dir = os.path.dirname(dest_files[0])

        for i in range(len(src_files)):
            cmd = Command('backup files', cmdStr='cp %s %s' % (src_files[i], cp_dir))
            self.pool.addCommand(cmd)

        self.pool.join()
        self.pool.check_results()

        dest_md5 = self.md5_validator.validate(dest_files)

        if src_md5 != dest_md5:
            raise Exception('MD5 sums do not match! Expected md5 = "%s", but actual md5 = "%s"' % (src_md5, dest_md5))

    def _copy_global_pt_files(self, restore=False):
        """
        Copies the global persistent table files to required directory
        If there is any error, we add it to the list of failures and return
        the list
        """
        failures = []
        for di in self.dbid_info:
            src_files, dest_files = [], []
            host, dbid = di.hostname, di.dbid 
            datadir = di.filespace_dirs[SYSTEM_FSOID] 
            for f in self.GLOBAL_PERSISTENT_FILES:
                src_files.append('%s/global/%s*' % (datadir, f)) 
                dest_files.append('%s/global/pt_files_bk_%s/%s*' % (datadir, self.timestamp, f))

            logger.debug('Source files = %s' % src_files)
            logger.debug('Destination files = %s' % dest_files)

            try:
                if not restore:
                    self._copy_pt_files(src_files, dest_files, host, restore)
                else:
                    self._copy_pt_files(dest_files, src_files, host, restore)
            except Exception as e:
                failures.append((di, str(e)))
                op = 'Backup' if not restore else 'Restore'
                logger.error('%s failed for segment %s: %s' % (op, di.filespace_dirs[SYSTEM_FSOID], str(e)))
        if failures:
            op = 'Backup' if not restore else 'Restore'
            raise Exception('%s of global persistent files failed' % op)

    def _copy_per_db_pt_files(self, restore=False):
        """
        Copies the per database persistent table files to required directory
        If there is any error, we add it to the list of failures and return
        the list
        """
        failures = []
        for di in self.dbid_info:
            host, dbid = di.hostname, di.dbid 
            for fsoid, fsdir in di.filespace_dirs.items():
                if fsoid not in di.fs_to_ts_map:
                    continue
                for tsoid in di.fs_to_ts_map[fsoid]:
                    if tsoid not in di.ts_to_dboid_map:
                        continue
                    for dboid in di.ts_to_dboid_map[tsoid]:
                        if fsoid != SYSTEM_FSOID:
                            base_dir = tsoid 
                        else:
                            base_dir = 'base'
                        src_files, dest_files = [], []
                        if dboid in self.PER_DATABASE_PERSISTENT_FILES:
                            file_list = self.PER_DATABASE_PERSISTENT_FILES[dboid]
                        else:
                            #This corresponds to template0. We cannot connect to template0
                            #hence the relfilenodeid will never change.
                            file_list = ['5094', '5095']
                        for f in file_list:
                            src_files.append('%s/%s/%s/%s*' % (fsdir, base_dir, dboid, f))
                            dest_files.append('%s/%s/%s/pt_files_bk_%s/%s*' % (fsdir, base_dir, dboid, self.timestamp, f))

                        logger.debug('Source files = %s' % src_files)
                        logger.debug('Destination files = %s' % dest_files)

                        try:
                            if not restore:
                                self._copy_pt_files(src_files, dest_files, host, restore)
                            else:
                                self._copy_pt_files(dest_files, src_files, host, restore)
                        except Exception as e:
                            failures.append((di, str(e)))
                            op = 'Backup' if not restore else 'Restore'
                            logger.error('%s of per database persistent files failed for %s: %s' % (op, di.filespace_dirs[SYSTEM_FSOID], str(e)))
        if failures:
            op = 'Backup' if not restore else 'Restore'
            raise Exception(' %s of per db persistent files failed' % op)

    def backup(self):
        try:
            self.pool = WorkerPool(self.batch_size)
            self.md5_validator = ValidateMD5Sum(self.pool, self.batch_size)
            self.md5_validator.init()
            logger.info('Backing up global persistent table files')
            self._copy_global_pt_files()
            logger.info('Backing up per database persistent table files')
            self._copy_per_db_pt_files()
        finally:
            self._cleanup_pool()

    def restore(self):
        try:
            self.pool = WorkerPool(self.batch_size)
            self.md5_validator = ValidateMD5Sum(self.pool, self.batch_size)
            self.md5_validator.init()
            logger.info('Restoring global persistent table files')
            self._copy_global_pt_files(restore=True)
            logger.info('Restoring per db persistent table files')
            self._copy_per_db_pt_files(restore=True)
        finally:
            self._cleanup_pool()

class RebuildTableOperation(Operation):
    """
    Run the sql functions to rebuild the persistent tables
    """
    def __init__(self, dbid_info, has_mirrors):
        self.dbid_info = dbid_info
        self.has_mirrors = has_mirrors

    def run(self):
        port = self.dbid_info.port
        hostname = self.dbid_info.hostname
        filerep_mirror = 'true' if self.has_mirrors else 'false'

        with dbconn.connect(dbconn.DbURL(dbname=DEFAULT_DATABASE, port=port), utility=True) as conn:
            dbconn.execSQL(conn, 'CHECKPOINT')
            conn.commit()
            logger.info('Finished checkpoint on %s' % self.dbid_info.filespace_dirs[SYSTEM_FSOID])
            dbconn.execSQL(conn, 'SELECT gp_persistent_reset_all()')
            conn.commit()
            logger.info('Completed gp_persistent_reset_all() on %s' % self.dbid_info.filespace_dirs[SYSTEM_FSOID])
            dbconn.execSQL(conn, 'SELECT gp_persistent_build_all(%s)' % filerep_mirror)
            conn.commit()
            logger.info('Completed gp_persistent_build_all() on %s' % self.dbid_info.filespace_dirs[SYSTEM_FSOID])
            dbconn.execSQL(conn, 'CHECKPOINT') 
            conn.commit()
            logger.info('Finished checkpoint on %s' % self.dbid_info.filespace_dirs[SYSTEM_FSOID])

class ValidatePersistentBackup:
    """
    Validate that the backup for persistent table files 
    are present before we acutally do the rebuild
    """
    def __init__(self, dbid_info, timestamp, batch_size=DEFAULT_BATCH_SIZE):
        self.dbid_info = dbid_info
        self.timestamp = timestamp
        self.batch_size = batch_size
        self.pool = None

    def _cleanup_pool(self):
        if self.pool:
            self.pool.haltWork()
            self.pool.joinWorkers()
            self.pool = None

    def _process_results(self, di, pool):
        err = False
        global_datadir = di.filespace_dirs[SYSTEM_FSOID]
        for item in pool.getCompletedItems():
            results = item.get_results()
            if not results.wasSuccessful():
                err = True
                logger.error('marking failure for content id %s:%s since it was not successful: %s' % (di.content, global_datadir, results))
            elif not results.stdout.strip():
                err = True
                logger.error('marking failure for content id %s:%s since backup was not found: %s' % (di.content, global_datadir, results))
        if err:
            raise Exception('Failed to validate backups')

    def validate(self):
        try:
            self.pool = WorkerPool(self.batch_size)
           
            for di in self.dbid_info:
                global_datadir = di.filespace_dirs[SYSTEM_FSOID]
                cmd = Command('Check if global pt backup exists', cmdStr='find %s/global -name pt_files_bk_%s' % (global_datadir, self.timestamp))
                self.pool.addCommand(cmd)
                for fsoid, fsdir in di.filespace_dirs.items(): 
                    if fsoid not in di.fs_to_ts_map:
                        continue
                    for tsoid in di.fs_to_ts_map[fsoid]:
                        if tsoid not in di.ts_to_dboid_map:
                            continue
                        for dboid in di.ts_to_dboid_map[tsoid]:
                            if fsoid == SYSTEM_FSOID:
                                base_dir = 'base'
                            else:
                                base_dir = tsoid
                            cmd = Command('Check if per db pt backup exists', cmdStr='find %s/base/%s -name pt_files_bk_%s' %
                                                                                     (fsdir, dboid, self.timestamp))
                self.pool.addCommand(cmd)
                self.pool.join()
                self._process_results(di, self.pool)
        finally:
            self._cleanup_pool()

class RebuildTable:
    """
    This class performs the following final checks before starting the rebuild process
    1. Check if the backup is present on the segment. In case a segment went down, 
       the mirror would take over and the backup might not be present on the mirror.
       Hence we do this check.
    2. Check if there are any contents that are down i.e Both primary and mirror are down.
    In either of the above two cases we chose to fail so that the user can fix the issue
    and rerun the tool.
    """
    def __init__(self, dbid_info, has_mirrors=False, batch_size=DEFAULT_BATCH_SIZE):
        self.gparray = None 
        self.dbid_info = dbid_info
        self.has_mirrors = False 
        self.batch_size = batch_size
        self.pool = None

    def _cleanup_pool(self):
        if self.pool:
            self.pool.haltWork()
            self.pool.joinWorkers()
            self.pool = None

    def _get_valid_dbids(self, content_ids):
        """Check to see if a content is down. i.e both primary and mirror"""
        valid_dbids = []
        for seg in self.gparray.getDbList():
            if seg.getSegmentContentId() in content_ids:
                if seg.getSegmentRole() == 'p' and seg.getSegmentStatus() == 'd':
                    raise Exception('Segment %s is down. Cannot continue with persistent table rebuild' % seg.getSegmentDataDirectory())
                elif seg.getSegmentRole() == 'p' and seg.getSegmentStatus() == 'r':
                    raise Exception('Segment %s is in resync. Cannot continue with persistent table rebuild' % seg.getSegmentDataDirectory())
                elif seg.getSegmentRole() == 'p' and seg.getSegmentStatus() != 'd':
                    valid_dbids.append(seg.getSegmentDbId())

        return valid_dbids 

    def _get_host_to_dbid_info_map(self, dbid_info):
        """
        Return a dictionary with key as the host 
        and the value as a list of DbIdInfo objects
        """
        host_to_dbid_info_map = defaultdict(list) 
        for di in dbid_info:
            host_to_dbid_info_map[di.hostname].append(di)
        return host_to_dbid_info_map

    def _validate_backups(self):
        RunBackupRestore(self.dbid_info, TIMESTAMP, self.batch_size).validate()

    def rebuild(self):
        """
        If any of the validations fail, we chose to error out.
        Otherwise we try to rebuild the persistent tables and return
        a list of successful dbidinfo and failed dbidinfo
        """
        self.gparray = GpArray.initFromCatalog(dbconn.DbURL(dbname=DEFAULT_DATABASE), utility=True)

        logger.info('Validating backups')
        self._validate_backups()

        logger.info('Validating dbids')
        content_ids = set([di.content for di in self.dbid_info])
        valid_dbids = self._get_valid_dbids(content_ids)
        valid_dbid_info = [di for di in self.dbid_info if di.dbid in valid_dbids] 
        successes, failures = [], []
        rebuild_done = {}
        operation_list = []

        logger.info('Starting persistent table rebuild operation')
        for di in valid_dbid_info:
            if di.content != -1:
                operation_list.append(RemoteOperation(RebuildTableOperation(di, self.has_mirrors), di.hostname))
            else:
                operation_list.append(RemoteOperation(RebuildTableOperation(di, False), di.hostname))

        try:
            ParallelOperation(operation_list, self.batch_size).run()
        except Exception as e:
            pass

        for op in operation_list:
            di = op.operation.dbid_info
            try:
                op.get_ret()
            except Exception as e:
                logger.debug('Table rebuild failed for content %s:%s ' % (di.content, str(e)))
                failures.append((di, str(e)))
            else:
                successes.append(di)
        return successes, failures

class ResetXlog:
    """
    This class runs pg_resetxlog. This has to be done
    on the primary and its corresponding mirror.
    We return a list of dbidinfo objects where we failed to 
    reset the xlog successfully.
    """
    def __init__(self, failures, gparray, batch_size=DEFAULT_BATCH_SIZE):
        self.failures = failures
        self.gparray = gparray
        self.batch_size = batch_size
        self.pool = None

    def reset(self):
        reset_failures = []
        content_to_mirror_map = {}
        for seg in self.gparray.getDbList():
            if seg.isSegmentMirror() or seg.isSegmentStandby():
                content_to_mirror_map[seg.getSegmentContentId()] = seg
        self.pool = WorkerPool(self.batch_size)
        for di, e in self.failures:
            cmd_list = []
            host, datadir = di.hostname, di.filespace_dirs[SYSTEM_FSOID]
            cmd_list.append(Command('reset pgxlog on the following segment', cmdStr='pg_resetxlog -f %s' % datadir,
                                    ctxt=REMOTE, remoteHost=host))
            if di.content in content_to_mirror_map:
                mirror_host, mirror_datadir = (content_to_mirror_map[di.content].getSegmentHostName(),
                                               content_to_mirror_map[di.content].getSegmentDataDirectory())
                cmd_list.append(Command('reset pgxlog on the following segment',
                                        cmdStr='pg_resetxlog -f %s' % mirror_datadir,
                                        ctxt=REMOTE, remoteHost=mirror_host))
            try:
                for cmd in cmd_list:
                    self.pool.addCommand(cmd) 
                self.pool.join()

                for item in self.pool.getCompletedItems():
                    result = item.get_results()
                    if not result.wasSuccessful():
                        reset_failures.append((di, result.stdout.strip()))
            finally:
                self.pool.haltWork()
                self.pool.joinWorkers()
                self.pool = None

        return reset_failures

class RunBackupRestore:
    """
    This is a wrapper class to invoke $GPHOME/sbin/gppersistent_backup.py
    """
    def __init__(self, dbid_info, timestamp, batch_size=DEFAULT_BATCH_SIZE):
        self.dbid_info = dbid_info
        self.timestamp = timestamp
        self.batch_size = batch_size
        self.pool = None

    def _get_host_to_dbid_info_map(self, dbid_info):
        host_to_dbid_info_map = defaultdict(list) 
        for di in dbid_info:
            host_to_dbid_info_map[di.hostname].append(di)
        return host_to_dbid_info_map

    def _process_results(self, pool, err_msg):
        err = False
        for completed_item in pool.getCompletedItems():
            res = completed_item.get_results()
            if res.rc != 0:
                err = True
                logger.error('************************************************')
                logger.error('%s' % res.stdout.strip())
                logger.error('************************************************')

        if err:
            raise Exception(err_msg)

    def _run_backup_restore(self, host_to_dbid_info_map, restore=False, validate=False):
        self.pool = WorkerPool(self.batch_size)
        if restore:
            option = '--restore'
            err_msg = 'Restore of persistent table files failed'
        elif validate:
            option = '--validate'
            err_msg = 'Validate of persistent table files failed'
        else:
            option = '--backup'
            err_msg = 'Backup of persistent table files failed'

        try:
            for host in host_to_dbid_info_map:
                pickled_backup_dbid_info = base64.urlsafe_b64encode(pickle.dumps(host_to_dbid_info_map[host]))
                pickled_per_database_persistent_files = base64.urlsafe_b64encode(pickle.dumps(PER_DATABASE_PERSISTENT_FILES))
                pickled_global_persistent_files = base64.urlsafe_b64encode(pickle.dumps(GLOBAL_PERSISTENT_FILES))
                cmdStr = """$GPHOME/sbin/gppersistent_backup.py --timestamp %s %s %s --batch-size %s --perdbpt %s --globalpt %s""" %\
                        (self.timestamp, option, pickled_backup_dbid_info, self.batch_size, pickled_per_database_persistent_files,
                         pickled_global_persistent_files)
                cmd = Command('backup pt files on a host', cmdStr=cmdStr, ctxt=REMOTE, remoteHost=host)
                self.pool.addCommand(cmd) 
            self.pool.join()
            self._process_results(self.pool, err_msg) 
        finally:
            self.pool.haltWork()
            self.pool.joinWorkers()
            self.pool = None

    def backup(self):
        host_to_dbid_info_map = self._get_host_to_dbid_info_map(self.dbid_info)
        self._run_backup_restore(host_to_dbid_info_map)

    def restore(self):
        host_to_dbid_info_map = self._get_host_to_dbid_info_map(self.dbid_info)
        self._run_backup_restore(host_to_dbid_info_map, restore=True)

    def validate(self):
        host_to_dbid_info_map = self._get_host_to_dbid_info_map(self.dbid_info)
        self._run_backup_restore(host_to_dbid_info_map, validate=True)

class RebuildPersistentTables(Operation):
    def __init__(self, content_id, contentid_file, backup, restore, batch_size):
        self.content_id = content_id
        self.contentid_file = contentid_file
        self.backup = backup
        self.restore = restore
        self.batch_size = batch_size
        self.content_info = None
        self.has_mirrors = None
        self.gparray = None
        self.pool = None

    def _check_database_version(self):
        """
        Checks if the database version is greater than or equal to 4.1.0.0
        since the gp_persistent_reset_all and gp_persistent_build_all is 
        not supported on earlier versions
        """
        if 'GPHOME' not in os.environ:
            raise Exception('GPHOME not set in the environment')
        gphome = os.environ['GPHOME']
        db_version = gpversion.GpVersion(GpVersion.local('get version', gphome)) 
        if db_version < gpversion.GpVersion('4.1.0.0'):
            raise Exception('This tool is not supported on Greenplum version lower than 4.1.0.0')

    def _stop_database(self):
        cmd = GpStop('Stop the greenplum database')
        cmd.run(validateAfter=True)

    def _start_database(self, admin_mode=False):
        """
        If admin_mode is set to True, it starts the database in
        admin mode. Since gpstart does not have an option to start
        in admin mode, we first start the entire database, then
        we stop the master only and restart it in utility mode
        so that it does not allow any connections
        """
        cmd = GpStart('Start the greenplum databse')
        cmd.run(validateAfter=True)
        if admin_mode:
            cmd = GpStop('Stop the greenplum database', masterOnly=True)
            cmd.run(validateAfter=True)
            cmd = GpStart('Start the greenplum master in admin mode', masterOnly=True)
            cmd.run(validateAfter=True)

    def _check_platform(self):
        """
        Solaris platform will be deprecated soon, hence we 
        choose to support it only on Linux and OSX.
        """
        operating_sys = platform.system()
        if operating_sys != 'Linux' and operating_sys != 'Darwin':
            raise Exception('This tool is only supported on Linux and OSX platforms')

    def _validate_has_mirrors(self):
        """
        Validate whether the system is configured with or without
        mirrors. This is required by gp_persistent_build_all function.
        If even a single segment does not have mirror, we consider that
        the entire system is configured without mirror. This does not
        apply to standby master
        """
        has_mirrors = False 
        for seg in self.gparray.getDbList():
            if seg.getSegmentContentId() != -1 and seg.isSegmentMirror(): 
                has_mirrors = True 
        return has_mirrors

    def _check_md5_prog(self):
        md5prog = 'md5' if platform.system() == 'Darwin' else 'md5sum'
        if not findCmdInPath(md5prog):
            raise Exception('Unable to find %s program. Please make sure it is in PATH' % md5prog)

    def _get_persistent_table_filenames(self):
        PER_DATABASE_PT_FILES_QUERY = """SELECT relfilenode FROM pg_class WHERE oid IN (5094, 5095)"""
        GLOBAL_PT_FILES_QUERY = """SELECT relfilenode FROM pg_class WHERE oid IN (5090, 5091, 5092, 5093)"""

        with dbconn.connect(dbconn.DbURL(dbname=DEFAULT_DATABASE)) as conn:
            res = dbconn.execSQL(conn, PER_DATABASE_PT_FILES_QUERY)
            for r in res:
                PER_DATABASE_PERSISTENT_FILES.append(r[0])
            res = dbconn.execSQL(conn, GLOBAL_PT_FILES_QUERY)
            for r in res:
                GLOBAL_PERSISTENT_FILES.append(r[0])

            res = dbconn.execSQL(conn, GET_ALL_DATABASES)
            for r in res:
                databases.append((r[0], r[1]))
    
        logger.debug('GLOBAL_PERSISTENT_FILES = %s' % GLOBAL_PERSISTENT_FILES)

        """
        We have to connect to each database to get the relfilenode ids for per db
        persistent files.
        """
        for dboid, database in databases:
            if database == 'template0': #Connections to template0 are not allowed so we skip
                continue
            with dbconn.connect(dbconn.DbURL(dbname=database)) as conn:
                res = dbconn.execSQL(conn, PER_DATABASE_PT_FILES_QUERY)
                for r in res:
                    PER_DATABASE_PERSISTENT_FILES[dboid].append(r[0])

        logger.debug('PER_DATABASE_PERSISTENT_FILES = %s' % PER_DATABASE_PERSISTENT_FILES)

    def print_warning(self):
        """
        Prints out a warning to the user indicating that this tool should 
        only be run by Pivotal support. It also asks for confirmation
        before proceeding. 
        """
        warning_msgs=  ['****************************************************', 'This tool should only be run by Pivotal support.',
                        'Please contact Pivotal support for more information.', '****************************************************']

        for warning_msg in warning_msgs:
            logger.warning(warning_msg)
        input = ask_yesno(None, 'Do you still wish to continue ?', 'N')
        if not input:
            raise Exception('Aborting rebuild due to user request') 

    def run(self):

        """
        Double warning to make sure that the customer 
        does not run this tool accidentally 
        """
        self.print_warning()
        self.print_warning()

        self.gparray = GpArray.initFromCatalog(dbconn.DbURL())

        logger.info('Checking for platform')
        self._check_platform()

        logger.info('Checking for md5sum program')
        self._check_md5_prog()

        logger.info('Checking for database version')
        self._check_database_version()

        logger.info('Validating if the system is configured with mirrors')
        self.has_mirrors = self._validate_has_mirrors()
        if self.has_mirrors:
            logger.info('System has been configured with mirrors')
        else:
            logger.info('System has been configured without mirrors')

        logger.info('Validating content ids')
        valid_contentids = ValidateContentID(self.content_id, self.contentid_file, self.gparray).validate()

        logger.info('Getting dbid information')
        self.dbid_info = GetDbIdInfo(self.gparray, valid_contentids).get_info()

        """
        We have to get the information about pt filenames from the master before we 
        do any backup since the database will be down when we do a backup and this 
        information is required in order to do the backup.
        """
        logger.info('Getting information about persistent table filenames')
        self._get_persistent_table_filenames()

        """
        We want to stop the database so that we can backup
        the persistent table files.
        """
        logger.info('Stopping Greenplum database')
        self._stop_database()

        """
        If the restore fails, we do not attempt to restart the database since a restore is only done
        when the PT rebuild has not succeeded. It might be dangerous to start the database when the 
        PT rebuild has failed in the middle and we cannot restore the original files safely.
        """
        if self.restore:
            logger.info('Restoring global and per database persistent table files from backup %s' % self.restore)
            try:
                RunBackupRestore(self.dbid_info, self.restore, self.batch_size).restore()
            except Exception as e:
                raise 
            else:
                logger.info('Starting Greenplum database')
                self._start_database()
                return            
   
        """
        If a backup fails, we still attempt to restart the database since the original files are 
        still present in their original location and we have not yet attempted to rebuild PT.  
        """
        logger.info('Backing up global and per database persistent table files')
        logger.info('Backup timestamp = %s' % TIMESTAMP)
        try:
            RunBackupRestore(self.dbid_info, TIMESTAMP, self.batch_size).backup()
        except Exception as e:
            logger.info('Starting Greenplum database')
            self._start_database()
            raise
        else:
            if self.backup:
                logger.info('Starting Greenplum database')
                self._start_database()
                return

        """
        All the PT rebuild should be performed in utility mode in order to prevent
        user activity during the PT rebuild
        """
        logger.info('Starting database in admin mode')
        self._start_database(admin_mode=True)

        """
        Since the PT rebuild was done in admin mode, we need to restore the database
        back to the normal mode once the PT rebuild is complete.
        """
        logger.info('Starting rebuild of persistent tables')
        try:
            successes, failures = RebuildTable(self.dbid_info, self.has_mirrors, self.batch_size).rebuild()
        finally:
            logger.info('Stopping Greenplum database that was started in admin mode')
            self._stop_database()
        
        reset_failures = []
        if failures:
            """
            If the PT rebuild failed for any reason, we need to reset the xlog in order to prevent
            the xlog replay otherwise the replay might overwrite the original PT files when we 
            replace them.
            """
            for f, e in failures:
                logger.error('************************************************')
                logger.error('Persistent Table rebuild failed for content %s: %s' % (f.filespace_dirs[SYSTEM_FSOID], str(e)))
                logger.error('************************************************')
            logger.info('Resetting Xlog on the above segments')
            reset_failures = ResetXlog(failures, self.gparray, self.batch_size).reset()

            """If resetxlog fails, we don't want to start the database and we error out"""
            if reset_failures:
                for f, e in reset_failures:
                    logger.error('************************************************')
                    logger.error('Reset pgxlog failed for content %s: %s' % (f.filespace_dirs[SYSTEM_FSOID], str(e)))
                    logger.error('************************************************')
                raise Exception("""Unable to reset pgxlog for failed persistent table rebuild!
                                 Please do no start the database without contacting support!! %s""" % reset_failures)

            logger.info('Restoring persistent table files')
            
            """If the restore of the original PT files fails, we want to error out"""
            RunBackupRestore([f for f, e in failures], TIMESTAMP, self.batch_size).restore()

        """
        If we reach this point, either PT rebuild has completed successfully
        or we have succesfully replaced the original PT files and reset the
        xlog. Hence it is safe to restart the database
        """
        logger.info('Starting Greenplum database')
        self._start_database()

        if failures:
            raise Exception('Persistent table rebuild was not completed succesfully')
        else:
            logger.info('Completed rebuild of persistent tables')

if __name__ == '__main__':
    pass
