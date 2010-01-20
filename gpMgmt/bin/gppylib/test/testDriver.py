import sys

from gppylib.gplog import *
from gppylib.interface.configurationInterface import *
from gppylib.implementationTest.configurationImplTest import ConfigurationProviderForTesting

logger = get_default_logger()

class TestDriver:

    def __init__(self):
        pass


    #
    #
    # To get segmentData you can run this query against a 3.4 database:
    #
    # SELECT dbid, content, role, preferred_role, mode, status, hostname, address, port,
    #                         fselocation AS datadir, replication_port
    #                     FROM pg_catalog.gp_segment_configuration
    #                         JOIN pg_catalog.pg_filespace_entry ON (dbid = fsedbid)
    #                         JOIN pg_catalog.pg_filespace fs ON (fsefsoid = fs.oid AND fsname='pg_system')
    #                    ORDER BY content, preferred_role DESC
    #
    # Include the header in what you paste!
    #
    #
    def setSegments(self, segmentData):
        lines = segmentData.strip().split("\n")
        
        assert len(lines[1].split("+")) == len(lines[0].split("|")) # verify header is listed

        conf = ConfigurationProviderForTesting()
        for line in lines[2:len(lines)]:
            row = [s.strip() for s in line.strip().split("|")]
            
            segment = ConfigurationForSegment()
            segment.setSegmentDbId(int(row[0]))
            segment.setSegmentContentId(int(row[1]))
            segment.setSegmentRole(row[2])
            segment.setSegmentPreferredRole(row[3])
            segment.setSegmentMode(row[4])
            segment.setSegmentStatus(row[5])
            segment.setSegmentHostName(row[6])
            segment.setSegmentAddress(row[7])
            segment.setSegmentPort(int(row[8]))
            segment.setSegmentDataDirectory(row[9])
            segment.setSegmentReplicationPort(None if row[10] == "" else int(row[10]))

            segment.resetSegmentDirtyBit()
            segment.markSegmentCreatedInDb()

            conf.addTestSegment(segment)
        registerConfigurationProvider( conf)
        pass

        
    def initOneHostConfiguration(self):
        configStr = \
            """
             dbid | content | role | preferred_role | mode | status |      hostname      |      address       | port  |                     datadir                     | replication_port
            ------+---------+------+----------------+------+--------+--------------------+--------------------+-------+-------------------------------------------------+------------------
            1 |      -1 | p    | p              | s    | u      | this-is-my-host | this-is-my-host |  5432 |/datadirpathdbmaster/gp-1 |
            2 |       0 | p    | p              | s    | u      | this-is-my-host | this-is-my-host | 50001 |/datadirpathdbfast1/gp0   |            55001
            4 |       0 | m    | m              | s    | u      | this-is-my-host | this-is-my-host | 60001 |/datadirpathdbfast3/gp0   |            65001
            3 |       1 | p    | p              | s    | u      | this-is-my-host | this-is-my-host | 50002 |/datadirpathdbfast2/gp1   |            55002
            5 |       1 | m    | m              | s    | u      | this-is-my-host | this-is-my-host | 60002 |/datadirpathdbfast4/gp1   |            65002
                
        """

        self.setSegments(configStr)
        return self
