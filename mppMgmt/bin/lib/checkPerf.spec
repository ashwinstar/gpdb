#
# This is a config file for checkPerf utility.
# Please follow the instructions at each section and name value pairs
#

#
# Section, General Configuration
#
[ "CheckPerf" ]
#
# Tolerance, variance allowed
#
Tolerance=float(0.01, 0.99, default=0.1)

# LogFile=string
#ResFile=string

#
# No of times each test should be performed.
#
RptCount=integer(1,20, default=5)
#
# Time in Seconds
#
ElapseTime=integer(0, default=0)

#
# Section Disk I/O Test
#
[ "Disk Performance"]
#
# Directory List, List of directories to test I/O performance.
# Format 
# Directory_List= "Path_to_dir1", "Path_to_dir2", "Path_to_dir2"
#
#Directory_List= string_list
#Directory_List= List
#
# Block Size in 1K Units
#
#
#BlockSize=integer(1, default=1)
BlockSize=string(default="1K")
#
# File Sizez
#
FileSize=string(default="0MB")
#
#  Count No of blocks
#
Count=integer(0, default=0)

#
# Section Memory Testing
#
["Memory Bandwidth"]
#
# No of concurrent tests that should be executed.
#
CONCURRENT_TESTS=integer(1, 10, default=5)


#
# Network Test
#
["Network Bandwidth"]
#
# Size of each Packet
#
PKT_SIZE=option(1024, 2046, 8204, default=1024)
#PKT_SIZE=option(1024, 2046, 8204)
#
# No of packets to be send
#
NUM_PKTS=integer(1, 1000, default=50)



