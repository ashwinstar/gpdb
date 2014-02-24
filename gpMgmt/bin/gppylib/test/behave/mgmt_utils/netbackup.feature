@netbackup
Feature: NetBackup Integration with GPDB

    Scenario: Valgrind test of gp_dump with netbackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "nbutestdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "nbutestdb" exists for validation
        When the user runs valgrind with "gp_dump --gp-d=db_dumps --gp-s=p --gp-c nbutestdb" and options " " and suppressions file "netbackup_suppressions.txt" using netbackup

    Scenario: Valgrind test of gp_dump with netbackup with table filter
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "nbutestdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "nbutestdb" exists for validation
        And the tables "public.heap_table" are in dirty hack file "/tmp/dirty_hack.txt"
        When the user runs valgrind with "gp_dump --gp-d=db_dumps --gp-s=p --gp-c --table-file=/tmp/dirty_hack.txt nbutestdb" and options " " and suppressions file "netbackup_suppressions.txt" using netbackup

    Scenario: Valgrind test of gp_restore with netbackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" table "ao_table" with compression "quicklz" in "nbutestdb" with data
        And there is a backupfile of tables "heap_table, ao_table" in "nbutestdb" exists for validation
        When the user runs "gpcrondump -a -x nbutestdb" using netbackup
        And gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the user runs valgrind with "gp_restore" and options "-i --gp-i --gp-l=p -d nbutestdb --gp-c" and suppressions file "netbackup_suppressions.txt" using netbackup

    Scenario: Valgrind test of gp_restore_agent with netbackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" table "ao_table" with compression "quicklz" in "nbutestdb" with data
        And there is a backupfile of tables "heap_table, ao_table" in "nbutestdb" exists for validation
        When the user runs "gpcrondump -a -x nbutestdb" using netbackup
        And gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the user runs valgrind with "gp_restore_agent" and options "--gp-c /bin/gunzip -s --post-data-schema-only --target-dbid 1 -d nbutestdb" and suppressions file "netbackup_suppressions.txt" using netbackup

    Scenario: Valgrind test of gp_dump_agent with table file using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" table "ao_table" with compression "quicklz" in "nbutestdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "None" in "nbutestdb" with data
        When the user runs valgrind with "./gppylib/test/behave/mgmt_utils/steps/scripts/valgrind_nbu_test_1.sh" and options " " and suppressions file "netbackup_suppressions.txt" using netbackup

    Scenario: Valgrind test of gp_bsa_dump_agent and gp_bsa_restore_agent
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        And there is a "ao" table "ao_table" with compression "quicklz" in "nbutestdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "None" in "nbutestdb" with data
        And the tables "public.heap_table" are in dirty hack file "/tmp/dirty_hack.txt"
        When the user runs backup command "cat /tmp/dirty_hack.txt | valgrind --suppressions=gppylib/test/behave/mgmt_utils/steps/netbackup_suppressions.txt gp_bsa_dump_agent --netbackup-filename /tmp/dirty_hack.txt" using netbackup
        And the user runs restore command "valgrind --suppressions=gppylib/test/behave/mgmt_utils/steps/netbackup_suppressions.txt gp_bsa_restore_agent --netbackup-filename /tmp/dirty_hack.txt" using netbackup

    @nbusmoke
    Scenario: gpcrondump using NetBackup with non-supported options
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a --incremental -x bkdb --netbackup-service-host=mdw --netbackup-policy=test_policy --netbackup-schedule=test_schedule"
        Then gpcrondump should print --incremental is not supported with NetBackup to stdout
        And gpcrondump should return a return code of 2
        And the user runs "gpcrondump -a --ddboost -x bkdb --netbackup-service-host=mdw --netbackup-policy=test_policy --netbackup-schedule=test_schedule"
        And gpcrondump should print --ddboost is not supported with NetBackup to stdout
        And gpcrondump should return a return code of 2

    @nbusmoke
    Scenario: gpdbrestore using NetBackup with non-supported options
        Given the database is running
        And the netbackup params have been parsed
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        When the user runs "gpcrondump -a -x nbutestdb" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs "gpdbrestore -e -s nbutestdb -a" using netbackup
        Then gpdbrestore should print -s is not supported with NetBackup to stdout
        And gpdbrestore should return a return code of 2
        When the user runs gpdbrestore with the stored timestamp and options "-b" using netbackup
        Then gpdbrestore should print -b is not supported with NetBackup to stdout
        And gpdbrestore should return a return code of 2
        When the user runs gpdbrestore with the stored timestamp and options "-L" using netbackup
        Then gpdbrestore should print -L is not supported with NetBackup to stdout
        And gpdbrestore should return a return code of 2

    Scenario: gpbrestore with -R using netbackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the backup files in "/tmp" are deleted
        And the database "nbutestdb" does not exist
        And database "nbutestdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "nbutestdb" with data
        When the user runs "gpcrondump -a -x nbutestdb -u /tmp" using netbackup
        Then gpcrondump should return a return code of 0
        And the subdir from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with "-R" option in path "/tmp" using netbackup
        Then gpdbrestore should print -R is not supported for restore with NetBackup to stdout

    ###
    ### Any tests that require this data set should come after
    ###
    @nbusmoke
    @nbudataset
    Scenario Outline: partition table test with data on all segments, with/without compression
        Given there is a "<tabletype>" partition table "<tablename>" with compression "<compressiontype>" in "<dbname>" with data
        Then data for partition table "<tablename>" with partition level "<partlevel>" is distributed across all segments on "<dbname>"
        And verify that partitioned tables "<tablename>" in "nbubkdb" have 6 partitions
        Examples:
        | tabletype | tablename           | compressiontype | dbname | partlevel |
        | ao        | ao_part_table       | None            | nbubkdb   | 1         |
        | co        | co_part_table       | None            | nbubkdb   | 1         |
        | heap      | heap_part_table     | None            | nbubkdb   | 1         |
        | ao        | ao_part_table_comp  | quicklz         | nbubkdb   | 1         |
        | co        | co_part_table_comp  | zlib            | nbubkdb   | 1         |

    @nbusmoke
    @nbudataset
    Scenario Outline: regular table test with data on all segments, with/without compression
        Given there is a "<tabletype>" table "<tablename>" with compression "<compressiontype>" in "<dbname>" with data
        Then data for table "<tablename>" is distributed across all segments on "<dbname>"
        Examples:
        | tabletype | tablename      | compressiontype | dbname |
        | ao        | ao_table       | None            | nbubkdb |
        | co        | co_table       | None            | nbubkdb |
        | heap      | heap_table     | None            | nbubkdb |
        | ao        | ao_table_comp  | quicklz         | nbubkdb |
        | co        | co_table_comp  | zlib            | nbubkdb |

    @nbusmoke
    @nbudataset
    Scenario Outline: regular table test with data on all segments, with/without compression and with indexes
        Given there is a "<tabletype>" table "<tablename>" with index "<indexname>" compression "<compressiontype>" in "<dbname>" with data
        Then data for table "<tablename>" is distributed across all segments on "<dbname>"
        Examples:
        | tabletype | tablename            | compressiontype | dbname | indexname      |
        | ao        | ao_index_table       | None            | nbubkdb   | ao_in          |
        | co        | co_index_table       | None            | nbubkdb   | co_in          |
        | heap      | heap_index_table     | None            | nbubkdb   | heap_in        |
        | ao        | ao_index_table_comp  | quicklz         | nbubkdb   | ao_comp_in     |
        | co        | co_index_table_comp  | zlib            | nbubkdb   | co_comp_in     |

    @nbusmoke
    @nbudataset
    Scenario: Partition tables with mixed storage types
        Given there is a mixed storage partition table "part_mixed_1" in "nbubkdb" with data
        Then data for partition table "part_mixed_1" with partition level "1" is distributed across all segments on "nbubkdb"
        And verify that storage_types of the partition table "part_mixed_1" are as expected in "nbubkdb"

    Scenario: Full Backup and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    @nbusmoke
    Scenario: Full Backup and Restore using NetBackup with --netbackup-block-size option
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options " --netbackup-block-size 1024 " using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    @nbusmoke
    Scenario: Full Backup and Restore using NetBackup with --netbackup-keyword option
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb --netbackup-keyword foo" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    Scenario: Full Backup and Restore using NetBackup with --netbackup-block-size and --netbackup-keyword options
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb --netbackup-block-size 4096 --netbackup-keyword foo" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options " --netbackup-block-size 4096 " using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    Scenario: Full Backup and Restore using NetBackup with keyword exceeding max limit of 100 characters
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb --netbackup-block-size 2048 --netbackup-keyword Aaaaaaaaaabbbbbbbbbbaaaaaaaaaabbbbbbbbbbaaaaaaaaaabbbbbbbbbbaaaaaaaaaabbbbbbbbbbaaaaaaaaaabbbbbbbbbbcccccc" using netbackup
        Then gpcrondump should return a return code of 2
        And gpcrondump should print NetBackup Keyword provided has more than max limit \(100\) characters. Cannot proceed with backup. to stdout

    @nbusmoke
    Scenario: Full Backup and Restore using NetBackup with -u option
        Given the database is running
        And the netbackup params have been parsed
        And the backup files in "/tmp" are deleted
        When the user runs "gpcrondump -a -x nbubkdb -u /tmp" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options " -u /tmp " using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    @nbusmoke
    Scenario: Full Backup and Restore with gp_dump using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gp_dump --gp-d=db_dumps --gp-s=p --gp-c nbubkdb" using netbackup
        Then gp_dump should return a return code of 0
        And the timestamp from gp_dump is stored and subdir is " "
        And all the data from "nbubkdb" is saved for verification
        And the database "nbubkdb" does not exist
        And database "nbubkdb" exists
        When the user runs gp_restore with the the stored timestamp and subdir in "nbubkdb" using netbackup
        Then gp_restore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    Scenario: gp_dump timestamp test using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb" using netbackup
        Then gpcrondump should return a return code of 0
        When the user runs "gp_dump --gp-d=db_dumps --gp-k=20131228111527 --gp-s=p --gp-c nbubkdb" using netbackup
        Then gp_dump should return a return code of 0
        And the timestamp from gp_dump is stored and subdir is " "
        And all the data from "nbubkdb" is saved for verification
        And the database "nbubkdb" does not exist
        And database "nbubkdb" exists
        When the user runs "gp_restore -i --gp-k 20131228111527 --gp-d db_dumps --gp-i --gp-r db_dumps --gp-l=p -d nbubkdb --gp-c" using netbackup
        Then gp_restore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    @nbusmoke
    Scenario: Full Backup with option -t and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb -t public.heap_table -t public.ao_part_table" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user truncates "public.heap_table" tables in "nbubkdb"
        And the user truncates "public.ao_part_table" tables in "nbubkdb"
        And the user runs gpdbrestore with the stored timestamp and options " " without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    @nbusmoke
    Scenario: Full Backup with option -T and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "public.heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "public.ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "co" partition table "public.co_part_table" with compression "quicklz" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb -T public.heap_table" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "18" tables in "fullbkdb" is validated after restore
        And verify that there is no table "heap_table" in "fullbkdb"

    Scenario: Full Backup with option -s and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is schema "schema_heap, schema_ao" exists in "fullbkdb"
        And there is a "heap" table "schema_heap.heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "schema_ao.ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "schema_heap.heap_table, schema_ao.ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb -s schema_heap --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "schema_heap.heap_table" in "fullbkdb" with data
        And verify that there is no table "schema_ao.ao_part_table" in "fullbkdb"

    Scenario: Full Backup with option -t and Restore after TRUNCATE on filtered tables using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is schema "schema_heap, schema_ao" exists in "fullbkdb"
        And there is a "heap" table "schema_heap.heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "schema_ao.ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "public.heap_table, public.ao_part_table" in "fullbkdb" exists for validation
        And there is a backupfile of tables "schema_heap.heap_table, schema_ao.ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb -t public.heap_table -t schema_ao.ao_part_table -t schema_heap.heap_table --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user truncates "public.heap_table" tables in "fullbkdb"
        And the user truncates "schema_heap.heap_table" tables in "fullbkdb"
        And the user truncates "schema_ao.ao_part_table" tables in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "--netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table" in "fullbkdb" with data
        And verify that there is a "heap" table "schema_heap.heap_table" in "fullbkdb" with data
        And verify that there is a "ao" table "schema_ao.ao_part_table" in "fullbkdb" with data
        And verify that there is a "ao" table "public.ao_part_table" in "fullbkdb" with data

    Scenario: Full Backup with option --exclude-table-file and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "co" partition table "co_part_table" with compression "None" in "fullbkdb" with data
        And there is a backupfile of tables "co_part_table" in "fullbkdb" exists for validation
        And there is a file "exclude_file" with tables "public.heap_table,public.ao_part_table"
        When the user runs "gpcrondump -a -x fullbkdb --exclude-table-file exclude_file --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "co" table "co_part_table" in "fullbkdb" with data
        And verify that there is no table "ao_part_table" in "fullbkdb"
        And verify that there is no table "heap_table" in "fullbkdb"

    @nbusmoke
    Scenario: Full Backup with option --table-file and Restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "co" partition table "co_part_table" with compression "None" in "fullbkdb" with data
        And there is a backupfile of tables "ao_part_table,heap_table" in "fullbkdb" exists for validation
        And there is a file "include_file" with tables "public.heap_table,public.ao_part_table"
        When the user runs "gpcrondump -a -x fullbkdb --table-file include_file --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is no table "co_part_table" in "fullbkdb"

    Scenario: Schema only restore of full backup using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        When the user runs "gpcrondump -a -x nbubkdb" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the table names in "nbubkdb" is stored
        And the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And tables names should be identical to stored table names in "nbubkdb"

    Scenario: Full Backup and Restore for Multiple Schemas with common tablenames using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is schema "pepper" exists in "schematestdb"
        And there is a "heap" table "pepper.heap_table" with compression "None" in "schematestdb" with data
        And there is a "heap" table "heap_table" with compression "None" in "schematestdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "schematestdb" with data
        And there is a "heap" partition table "pepper.heap_part_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "pepper.ao_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "pepper.ao_table2" with compression "None" in "schematestdb" with data
        And there is a "ao" table "ao_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "ao_table2" with compression "None" in "schematestdb" with data
        And there is a "co" partition table "pepper.co_part_table" with compression "quicklz" in "schematestdb" with data
        And there is a "co" partition table "co_part_table" with compression "quicklz" in "schematestdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "schematestdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "34" tables in "schematestdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "schematestdb"

    @nbusmoke
    Scenario: Full Backup and Restore without compression using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -x nbubkdb -z --netbackup-block-size 1024 --netbackup-keyword foo" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "nbubkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "68" tables in "nbubkdb" is validated after restore

    Scenario: Out of Sync timestamp using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is a "ao" table "ao_table" with compression "None" in "schematestdb" with data
        And there is a "co" table "co_table" with compression "None" in "schematestdb" with data
        And there are no backup files
        # this test will break after 2021 year
        And there is a fake timestamp for "20210130093000"
        When the user runs "gpcrondump -a -x schematestdb" using netbackup
        Then gpcrondump should return a return code of 2
        And gpcrondump should print There is a future dated backup on the system preventing new backups to stdout

    Scenario: Verify the gpcrondump -o option is not broken due to NetBackup changes
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is schema "pepper" exists in "schematestdb"
        And there is a "ao" table "pepper.ao_table" with compression "None" in "schematestdb" with data
        And there is a "co" table "pepper.co_table" with compression "None" in "schematestdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb" using netbackup
        Then gpcrondump should return a return code of 0
        And the full backup timestamp from gpcrondump is stored
        And older backup directories "20130101" exists
        When the user runs "gpcrondump -o" using netbackup
        Then gpcrondump should return a return code of 0
        And the dump directories "20130101" should not exist
        And the dump directory for the stored timestamp should exist

    Scenario: Verify the gpcrondump -c option is not broken due to NetBackup changes
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is schema "pepper" exists in "schematestdb"
        And there is a "ao" table "pepper.ao_table" with compression "None" in "schematestdb" with data
        And there is a "co" table "pepper.co_table" with compression "None" in "schematestdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb" using netbackup
        Then gpcrondump should return a return code of 0
        And the full backup timestamp from gpcrondump is stored
        And older backup directories "20130101" exists
        When the user runs "gpcrondump -c -x schematestdb -a" using netbackup
        Then gpcrondump should return a return code of 0
        And the dump directories "20130101" should not exist
        And the dump directory for the stored timestamp should exist

    Scenario: Verify the gpcrondump -g option is not broken due to NetBackup changes
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb -g" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And config files should be backed up on all segments

    Scenario: Verify the gpcrondump -h option works with full backup using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is schema "pepper" exists in "schematestdb"
        And there is a "ao" table "pepper.ao_table" with compression "None" in "schematestdb" with data
        And there is a "co" table "pepper.co_table" with compression "None" in "schematestdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb -h" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that there is a "heap" table "gpcrondump_history" in "schematestdb"
        And verify that the table "gpcrondump_history" in "schematestdb" has dump info for the stored timestamp

    Scenario: gpdbrestore -u option with full backup timestamp using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "co_table" with compression "None" in "testdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x testdb -u /tmp" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When there are no backup files
        And the user runs gpdbrestore with the stored timestamp and options "-u /tmp --verbose" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "2" tables in "testdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb"

    Scenario: gpcrondump -x with multiple databases using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb1" does not exist
        And the database "testdb2" does not exist
        And database "testdb1" exists
        And database "testdb2" exists
        And there is a "ao" table "ao_table1" with compression "None" in "testdb1" with data
        And there is a "co" table "co_table1" with compression "None" in "testdb1" with data
        And there is a "ao" table "ao_table2" with compression "None" in "testdb2" with data
        And there is a "co" table "co_table2" with compression "None" in "testdb2" with data
        And there are no backup files
        And there is a list to store the backup timestamps
        When the user runs "gpcrondump -a -x testdb1,testdb2 --netbackup-block-size 2048" using netbackup
        And the timestamp for database dumps "testdb1, testdb2" are stored
        Then gpcrondump should return a return code of 0
        And all the data from "testdb1" is saved for verification
        And all the data from "testdb2" is saved for verification
        When the user runs gpdbrestore for the database "testdb1" with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs gpdbrestore for the database "testdb2" with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "2" tables in "testdb1" is validated after restore
        And verify that the data of "2" tables in "testdb2" is validated after restore
        And the dump timestamp for "testdb1, testdb2" are different
        And verify that the tuple count of all appendonly tables are consistent in "testdb1"
        And verify that the tuple count of all appendonly tables are consistent in "testdb2"

    Scenario: gpdbrestore with -T and --table-file should fail using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb1" does not exist
        And database "testdb1" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb1" with data
        And there is a "co" table "co_table" with compression "None" in "testdb1" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x testdb1" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "-T public.ao_table --table-file foo" using netbackup
        Then gpdbrestore should return a return code of 2
        And gpdbrestore should print Cannot specify -T and --table-file together to stdout

    Scenario: gpdbrestore with --table-file option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb1" does not exist
        And database "testdb1" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb1" with data
        And there is a "co" table "co_table" with compression "None" in "testdb1" with data
        And there are no backup files
        And there is a table-file "/tmp/table_file_foo" with tables "public.ao_table, public.co_table"
        When the user runs "gpcrondump -a -x testdb1" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb1" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--table-file /tmp/table_file_foo" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "2" tables in "testdb1" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb1"

    Scenario: Multiple full backup and restore from first full backup using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "schematestdb" does not exist
        And database "schematestdb" exists
        And there is schema "pepper" exists in "schematestdb"
        And there is a "heap" table "pepper.heap_table" with compression "None" in "schematestdb" with data
        And there is a "heap" table "heap_table" with compression "None" in "schematestdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "schematestdb" with data
        And there is a "heap" partition table "pepper.heap_part_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "pepper.ao_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "pepper.ao_table2" with compression "None" in "schematestdb" with data
        And there is a "ao" table "ao_table" with compression "None" in "schematestdb" with data
        And there is a "ao" table "ao_table2" with compression "None" in "schematestdb" with data
        And there is a "co" partition table "pepper.co_part_table" with compression "quicklz" in "schematestdb" with data
        And there is a "co" partition table "co_part_table" with compression "quicklz" in "schematestdb" with data
        And there are no backup files
        When the user runs "gpcrondump -a -x schematestdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "schematestdb" is saved for verification
        When the numbers "1" to "100" are inserted into "pepper.ao_table" tables in "schematestdb"
        And the user runs "gpcrondump -a -x schematestdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "34" tables in "schematestdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "schematestdb"

    Scenario: Full backup and restore with external tables using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb1" does not exist
        And database "testdb1" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb1" with data
        And there is a "co" table "co_table" with compression "None" in "testdb1" with data
        And there is an external table "ext_tab" in "testdb1" with data for file "/tmp/ext_tab"
        And there are no backup files
        When the user runs "gpcrondump -a -x testdb1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb1" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "3" tables in "testdb1" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb1"
        And the file "/tmp/ext_tab" is removed from the system

    Scenario: gpdbrestore with -T option using netbackup 1
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options " -T public.ao_index_table -a --netbackup-block-size 2048" using netbackup
        And gpdbrestore should return a return code of 0
        Then verify that there is no table "ao_part_table" in "fullbkdb"
        And verify that there is no table "heap_table" in "fullbkdb"
        And verify that there is a "ao" table "public.ao_index_table" in "fullbkdb" with data

    Scenario: gpdbrestore with -T option using netbackup 2
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        When the user runs gpdbrestore with the stored timestamp and options "-T public.ao_index_table -a --netbackup-block-size 2048" without -e option using netbackup
        And gpdbrestore should return a return code of 0
        Then verify that there is no table "ao_part_table" in "fullbkdb"
        And verify that there is no table "heap_table" in "fullbkdb"
        And verify that there is a "ao" table "public.ao_index_table" in "fullbkdb" with data

    Scenario: gpdbrestore with -T option using netbackup 3
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user truncates "public.ao_index_table" tables in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.ao_index_table -a --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is no table "ao_part_table" in "fullbkdb"
        And verify that there is no table "heap_table" in "fullbkdb"
        And verify that there is a "ao" table "public.ao_index_table" in "fullbkdb" with data

    Scenario: gpdbrestore with -T option using netbackup 4
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When table "public.ao_index_table" is deleted in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.ao_index_table -a --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is no table "ao_part_table" in "fullbkdb"
        And verify that there is no table "heap_table" in "fullbkdb"
        And verify that there is a "ao" table "public.ao_index_table" in "fullbkdb" with data

    Scenario: gpdbrestore with -T option using netbackup 5
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user truncates "public.ao_part_table_1_prt_p2_2_prt_3" tables in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.ao_part_table_1_prt_p2_2_prt_3 -a --netbackup-block-size 2048" without -e option using netbackup
        And gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_part_table_1_prt_p2_2_prt_3" in "fullbkdb" with data

    Scenario: gpdbrestore with -T option using netbackup 6
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user truncates "public.ao_part_table_1_prt_p2_2_prt_3" tables in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T ao_part_table_1_prt_p2_2_prt_3 -a --netbackup-block-size 2048" without -e option using netbackup
        And gpdbrestore should return a return code of 2
        Then gpdbrestore should print No schema name supplied to stdout

    Scenario: Config files backup using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb -g --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that the config files are backed up with the stored timestamp using netbackup

    Scenario: Test gpcrondump and gpdbrestore verbose option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" table "ao_table" with compression "quicklz" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --verbose --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--verbose --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "2" tables in "fullbkdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "fullbkdb"

    Scenario: gpdbrestore list_backup option with full timestamp using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "bkdb" does not exist
        And database "bkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "bkdb" with data
        When the user runs "gpcrondump -a -x bkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp to print the backup set with options "--netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 2
        And gpdbrestore should print --list-backup is not supported for restore with full timestamps to stdout

    Scenario: Full Backup with option -t and non-existant table using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb -t cool.dude -t public.heap_table" using netbackup
        Then gpcrondump should return a return code of 2
        And gpcrondump should print does not exist in to stdout

    Scenario: Full Backup with option -T and non-existant table using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb -T public.heap_table -T cool.dude --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And gpcrondump should print does not exist in to stdout
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data
        And verify that there is no table "heap_table" in "fullbkdb"

    Scenario: Funny character table names using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_table" with funny characters in "testdb"
        And there is a "co" table "public.co_table" with funny characters in "testdb"
        And there are no backup files
        When the user runs "gpcrondump -a -x testdb" using netbackup
        Then gpcrondump should return a return code of 2
        And gpcrondump should print Tablename has an invalid character ".n", ":", "," : to stdout

    @nbusmoke
    Scenario: gp_restore with no ao stats using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "co" partition table "co_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table, co_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        When the user runs "gp_dump --gp-d=db_dumps --gp-s=p --gp-c fullbkdb --netbackup-block-size 2048" using netbackup
        Then gp_dump should return a return code of 0
        And the timestamp from gp_dump is stored and subdir is " "
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        When the user runs gp_restore with the the stored timestamp and subdir in "fullbkdb" and bypasses ao stats using netbackup
        Then gp_restore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data
        And verify that there is a "co" table "co_part_table" in "fullbkdb" with data
        And verify that there are no aoco stats in "fullbkdb" for table "ao_part_table_1_prt_p1_2_prt_1, ao_part_table_1_prt_p1_2_prt_2, ao_part_table_1_prt_p1_2_prt_3"
        And verify that there are no aoco stats in "fullbkdb" for table "ao_part_table_1_prt_p2_2_prt_1, ao_part_table_1_prt_p2_2_prt_2, ao_part_table_1_prt_p2_2_prt_3"
        And verify that there are no aoco stats in "fullbkdb" for table "co_part_table_1_prt_p1_2_prt_1, co_part_table_1_prt_p1_2_prt_2, co_part_table_1_prt_p1_2_prt_3"
        And verify that there are no aoco stats in "fullbkdb" for table "co_part_table_1_prt_p2_2_prt_1, co_part_table_1_prt_p2_2_prt_2, co_part_table_1_prt_p2_2_prt_3"

    Scenario: Negative tests for table filter options for gpcrondump using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        When the user runs "gpcrondump -a -t public.table --table-file include_file -x schematestdb" using netbackup
        Then gpcrondump should print -t can not be selected with --table-file option to stdout
        And gpcrondump should return a return code of 2
        When the user runs "gpcrondump -a -t public.table --exclude-table-file exclude_file -x schematestdb" using netbackup
        Then gpcrondump should print -t can not be selected with --exclude-table-file option to stdout
        And gpcrondump should return a return code of 2
        When the user runs "gpcrondump -a -T public.table --exclude-table-file exclude_file -x schematestdb" using netbackup
        Then gpcrondump should print -T can not be selected with --exclude-table-file option to stdout
        And gpcrondump should return a return code of 2
        When the user runs "gpcrondump -a -T public.table --table-file include_file -x schematestdb" using netbackup
        Then gpcrondump should print -T can not be selected with --table-file option to stdout
        And gpcrondump should return a return code of 2
        When the user runs "gpcrondump -a -t public.table1 -T public.table2 -x schematestdb" using netbackup
        Then gpcrondump should print -t can not be selected with -T option to stdout
        And gpcrondump should return a return code of 2
        When the user runs "gpcrondump -a --table-file include_file --exclude-table-file exclude_file -x schematestdb" using netbackup
        Then gpcrondump should print --table-file can not be selected with --exclude-table-file option to stdout
        And gpcrondump should return a return code of 2

    Scenario: User specified timestamp key for dump using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb -K 20130101010101 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that report file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that state file with prefix " " under subdir " " has been backed up using NetBackup
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "11" tables in "testdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb"

    Scenario: User specified invalid timestamp key for dump using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb -K 201301010101" using netbackup
        Then gpcrondump should return a return code of 2
        And gpcrondump should print Invalid timestamp key to stdout

    ######## Add test for --list-netbackup-files option
    ######## Scenario: Full Backup with --list-netbackup-files and --prefix options using NetBackup
    ######## Add scenario for invalid netbackup params

    ######## Restore with NetBackup using invalid timestamp - error message can be improved
    Scenario: User specified invalid timestamp key for restore using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb -K 20130101010101" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs "gpdbrestore -t 20130101010102 -a -e" using netbackup
        Then gpdbrestore should return a return code of 2
        And gpcrondump should print Failed to get the NetBackup BSA restore object to perform Restore to stdout

    @nbusmoke
    Scenario: Full Backup and Restore with --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "testdb" exists for validation
        When the user runs "gpcrondump -a -x testdb --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that report file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that state file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that there is a "heap" table "heap_table" in "testdb" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb" with data

    Scenario: Full Backup and Restore with --prefix option for multiple databases using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb1" does not exist
        And database "testdb1" exists
        And the database "testdb2" does not exist
        And database "testdb2" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb1" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb1" with data
        And there is a backupfile of tables "heap_table1, ao_part_table" in "testdb1" exists for validation
        And there is a "heap" table "heap_table2" with compression "None" in "testdb2" with data
        And there is a backupfile of tables "heap_table2" in "testdb2" exists for validation
        When the user runs "gpcrondump -a -x testdb1,testdb2 --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that report file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that state file with prefix "foo" under subdir " " has been backed up using NetBackup
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table1" in "testdb1" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb1" with data
        And verify that there is a "heap" table "heap_table2" in "testdb2" with data

    Scenario: Full Backup and Restore with -g and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "testdb" exists for validation
        When the user runs "gpcrondump -a -x testdb --prefix=foo -g --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And config files should be backed up on all segments
        And verify that report file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that state file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that config file with prefix "foo" under subdir " " has been backed up using NetBackup
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "testdb" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb" with data

    Scenario: Full Backup and Restore with -G and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "testdb" exists for validation
        When the user runs "gpcrondump -a -x testdb --prefix=foo -G --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that report file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that state file with prefix "foo" under subdir " " has been backed up using NetBackup
        And verify that global file with prefix "foo" under subdir " " has been backed up using NetBackup
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "testdb" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb" with data

    Scenario: Full Backup and Restore with -u and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the backup files in "/tmp" are deleted
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "testdb" exists for validation
        When the user runs "gpcrondump -a -x testdb --prefix=foo -u /tmp --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the subdir from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        And verify that report file with prefix "foo" under subdir "/tmp" has been backed up using NetBackup
        And verify that cdatabase file with prefix "foo" under subdir "/tmp" has been backed up using NetBackup
        And verify that state file with prefix "foo" under subdir "/tmp" has been backed up using NetBackup
        When the user runs gpdbrestore with the stored timestamp and options "-u /tmp --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "testdb" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb" with data

    Scenario: Multiple Full Backup and Restore with and without prefix using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        When the user runs "gpcrondump -a -x testdb --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "11" tables in "testdb" is validated after restore

    Scenario: Multiple Full Backup and Restore with different prefixes using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a list to store the backup timestamps
        And the database "testdb1" does not exist
        And the database "testdb2" does not exist
        And database "testdb1" exists
        And database "testdb2" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb1" with data
        And there is a "ao" table "ao_index_table1" with compression "None" in "testdb1" with data
        And there is a "ao" partition table "ao_part_table1" with compression "quicklz" in "testdb1" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb2" with data
        And there is a "ao" table "ao_index_table2" with compression "None" in "testdb2" with data
        And there is a "ao" partition table "ao_part_table2" with compression "quicklz" in "testdb2" with data
        When the user runs "gpcrondump -a -x testdb1 --prefix=foo1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored in the backup timestamp list
        When the user runs "gpcrondump -a -x testdb2 --prefix=foo2 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored in the backup timestamp list
        And all the data from "testdb1" is saved for verification
        And all the data from "testdb2" is saved for verification
        When the user runs gpdbrestore with the backup list stored timestamp and options "--prefix=foo1 --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs gpdbrestore with the backup list stored timestamp and options "--prefix=foo2 --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "11" tables in "testdb1" is validated after restore
        And verify that the data of "11" tables in "testdb2" is validated after restore

    Scenario: Restore database without prefix for a dump with prefix using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 2
        And gpdbrestore should print No object matched the specified predicate to stdout

    Scenario: Full Backup and Restore with -t filter and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --prefix=foo -t public.ao_index_table -t public.heap_table1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table1" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is no table "public.ao_part_table" in "testdb"
        And verify that there is no table "public.heap_table2" in "testdb"

    Scenario: Full Backup and Restore with -T filter and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --prefix=foo -T public.ao_part_table -T public.heap_table2 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table1" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is no table "public.ao_part_table" in "testdb"
        And verify that there is no table "public.heap_table2" in "testdb"

    Scenario: Full Backup and Restore with --table-file filter and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a table-file "/tmp/table_file_1" with tables "public.ao_index_table, public.heap_table1"
        When the user runs "gpcrondump -a -x testdb --prefix=foo --table-file /tmp/table_file_1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table1" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is no table "public.ao_part_table" in "testdb"
        And verify that there is no table "public.heap_table2" in "testdb"

    Scenario: Full Backup and Restore with --exclude-table-file filter and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a table-file "/tmp/exclude_table_file_1" with tables "public.ao_part_table, public.heap_table2"
        When the user runs "gpcrondump -a -x testdb --prefix=foo --exclude-table-file /tmp/exclude_table_file_1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table1" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is no table "public.ao_part_table" in "testdb"
        And verify that there is no table "public.heap_table2" in "testdb"

    Scenario: Full Backup and Restore with Multiple Schemas and -t filter and --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "pepper" exists in "testdb"
        And there is a "heap" table "pepper.heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "pepper.heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "pepper.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "pepper.co_table" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table1" with compression "None" in "testdb" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "testdb" with data
        And there is a "co" table "co_index_table" with compression "None" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb -K 20120101010101 --prefix=foo -t public.ao_index_table -t public.heap_table1 -t pepper.ao_table -t pepper.heap_table1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user truncates "public.ao_index_table, public.heap_table1" tables in "testdb"
        And the user truncates "pepper.ao_table, pepper.heap_table1" tables in "testdb"
        And the user runs "gpdbrestore -a -t 20120101010101 --prefix=foo --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table1" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table2" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is a "co" table "public.co_index_table" in "testdb" with data
        And verify that there is a "heap" table "pepper.heap_table1" in "testdb" with data
        And verify that there is a "heap" table "pepper.heap_table2" in "testdb" with data
        And verify that there is a "ao" table "pepper.ao_table" in "testdb" with data
        And verify that there is a "co" table "pepper.co_table" in "testdb" with data

    Scenario: Multiple Full Backup and Restore with different prefixes and -K option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a list to store the backup timestamps
        And the database "testdb1" does not exist
        And the database "testdb2" does not exist
        And database "testdb1" exists
        And database "testdb2" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb1" with data
        And there is a "ao" table "ao_index_table1" with compression "None" in "testdb1" with data
        And there is a "ao" partition table "ao_part_table1" with compression "quicklz" in "testdb1" with data
        And there is a "heap" table "heap_table2" with compression "None" in "testdb2" with data
        And there is a "ao" table "ao_index_table2" with compression "None" in "testdb2" with data
        And there is a "ao" partition table "ao_part_table2" with compression "quicklz" in "testdb2" with data
        When the user runs "gpcrondump -a -x testdb1 -K 20120101010101 --prefix=foo1 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored in the backup timestamp list
        When the user runs "gpcrondump -a -x testdb2 -K 20130101010101 --prefix=foo2 --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored in the backup timestamp list
        And all the data from "testdb1" is saved for verification
        And all the data from "testdb2" is saved for verification
        When the user runs gpdbrestore with the backup list stored timestamp and options "--prefix=foo1 --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs gpdbrestore with the backup list stored timestamp and options "--prefix=foo2 --netbackup-block-size 2048" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "11" tables in "testdb1" is validated after restore
        And verify that the data of "11" tables in "testdb2" is validated after restore

    Scenario: Gpcrondump with no PGPORT set using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        And the environment variable "PGPORT" is not set
        When the user runs "gpcrondump -G -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the environment variable "PGPORT" is reset
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data

    Scenario: Checking for abnormal whitespace using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "co" partition table "co_part_table" with compression "None" in "fullbkdb" with data
        And there is a file "include_file_with_whitespace" with tables "public.heap_table   ,public.ao_part_table"
        And there is a backupfile of tables "heap_table,ao_part_table" in "fullbkdb" exists for validation
        When the user runs "gpcrondump -a -x fullbkdb --table-file include_file_with_whitespace --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is no table "co_part_table" in "fullbkdb"

    Scenario: Full Backup and Restore of all tables with -C option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb -C --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "fullbkdb"
        And verify that there is a "ao" table "ao_table" in "fullbkdb"
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb"

    Scenario: Full Backup and Restore with filtering tables within public and non-public schema using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "pepper" exists in "testdb"
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "heap" partition table "pepper.heap_part_table" with compression "quicklz" in "testdb" with data
        And there is a "ao" table "pepper.ao_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "public.ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a "co" table "public.co_table" with compression "None" in "testdb" with data
        And there is a "co" partition table "public.co_part_table" with compression "quicklz" in "testdb" with data
        And there is a "heap" table "pepper.heap_table_ex" with compression "None" in "testdb" with data
        And there is a "heap" partition table "public.heap_part_table_ex" with compression "quicklz" in "testdb" with data
        And there is a "ao" table "public.ao_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" partition table "public.ao_part_table_ex" with compression "quicklz" in "testdb" with data
        And there is a "co" table "pepper.co_table_ex" with compression "None" in "testdb" with data
        And there is a "co" partition table "pepper.co_part_table_ex" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And there is a file "restore_file" with tables "public.heap_table,pepper.ao_table,public.co_table"
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "pepper" exists in "testdb"
        When the user runs gpdbrestore with the stored timestamp and options "--table-file restore_file --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "ao" table "pepper.ao_table" in "testdb" with data
        And verify that there is a "co" table "public.co_table" in "testdb" with data
        And verify that there is no table "pepper.heap_table_ex" in "testdb"
        And verify that there is no table "public.ao_table_ex" in "testdb"
        And verify that there is no table "pepper.co_table_ex" in "testdb"
        And verify that there is no table "public.heap_part_table_ex" in "testdb"
        And verify that there is no table "public.ao_part_table_ex" in "testdb"
        And verify that there is no table "pepper.co_part_table_ex" in "testdb"
        And verify that there is no table "pepper.heap_part_table" in "testdb"
        And verify that there is no table "public.ao_part_table" in "testdb"
        And verify that there is no table "public.co_part_table" in "testdb"
        And verify that the data of "3" tables in "testdb" is validated after restore

    Scenario: Full Backup and Restore with filtering tables from multiple schemas using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "pepper" exists in "testdb"
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "pepper.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "pepper.co_table_ex" with compression "None" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "pepper" exists in "testdb"
        When the user runs gpdbrestore with the stored timestamp and options "-T pepper.ao_table --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "pepper.ao_table" in "testdb" with data
        And verify that there is no table "public.heap_table" in "testdb"
        And verify that there is no table "pepper.co_table_ex" in "testdb"
        And verify that the data of "1" tables in "testdb" is validated after restore

    Scenario: Full Backup and Restore of external and ao table using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "public.co_table_ex" with compression "None" in "testdb" with data
        And there is an external table "ext_tab" in "testdb" with data for file "/tmp/ext_tab"
        And the user runs "psql -c 'CREATE LANGUAGE plpythonu' testdb"
        And there is a function "pymax" in "testdb"
        And the user runs "psql -c 'CREATE VIEW vista AS SELECT text 'Hello World' AS hello' testdb"
        And the user runs "psql -c 'COMMENT ON  TABLE public.ao_table IS 'Hello World' testdb"
        And the user runs "psql -c 'CREATE ROLE foo_user' testdb"
        And the user runs "psql -c 'GRANT INSERT ON TABLE public.ao_table TO foo_user' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY public.ao_table ADD CONSTRAINT null_check CHECK (column1 <> NULL);' testdb"
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a file "restore_file" with tables "public.ao_table,public.ext_tab"
        When the user runs gpdbrestore with the stored timestamp and options "--table-file restore_file --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_table" in "testdb" with data
        And verify that there is a "external" table "public.ext_tab" in "testdb" with data
        And verify that there is a constraint "null_check" in "testdb"
        And verify that there is no table "public.heap_table" in "testdb"
        And verify that there is no table "public.co_table_ex" in "testdb"
        And verify that there is no view "vista" in "testdb"
        And verify that there is no procedural language "plpythonu" in "testdb"
        And the user runs "psql -c '\z' testdb"
        And psql should print foo_user=a/ to stdout
        And the user runs "psql -c '\df' testdb"
        And psql should not print pymax to stdout
        And verify that the data of "2" tables in "testdb" is validated after restore

    Scenario: Full Backup and Restore filtering tables with post data objects using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "heap" table "public.heap_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "public.co_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_index_table" with index "ao_index" compression "None" in "testdb" with data
        And there is a "co" table "public.co_index_table" with index "co_index" compression "None" in "testdb" with data
        And there is a trigger "heap_trigger" on table "public.heap_table" in "testdb" based on function "heap_trigger_func"
        And there is a trigger "heap_ex_trigger" on table "public.heap_table_ex" in "testdb" based on function "heap_ex_trigger_func"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table_ex ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY heap_table ADD CONSTRAINT heap_const_1 FOREIGN KEY (column1, column2, column3) REFERENCES heap_table_ex(column1, column2, column3);' testdb"
        And the user runs "psql -c """create rule heap_co_rule as on insert to heap_table where column1=100 do instead insert into co_table_ex values(27, 'restore', '2013-08-19');""" testdb"
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And there is a file "restore_file" with tables "public.ao_table,public.ao_index_table,public.heap_table"
        When table "public.ao_index_table" is dropped in "testdb"
        And table "public.ao_table" is dropped in "testdb"
        And table "public.heap_table" is dropped in "testdb"
        And the index "bitmap_co_index" in "testdb" is dropped
        And the trigger "heap_ex_trigger" on table "public.heap_table_ex" in "testdb" is dropped
        When the user runs gpdbrestore with the stored timestamp and options "--table-file restore_file --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table_ex" in "testdb" with data
        And verify that there is a "co" table "public.co_table_ex" in "testdb" with data
        And verify that there is a "co" table "public.co_index_table" in "testdb" with data
        And the user runs "psql -c '\d public.ao_index_table' testdb"
        And psql should print \"bitmap_ao_index\" bitmap \(column3\) to stdout
        And psql should print \"btree_ao_index\" btree \(column1\) to stdout
        And the user runs "psql -c '\d public.heap_table' testdb"
        And psql should print \"heap_table_pkey\" PRIMARY KEY, btree \(column1, column2, column3\) to stdout
        And psql should print heap_trigger AFTER INSERT OR DELETE OR UPDATE ON heap_table FOR EACH STATEMENT EXECUTE PROCEDURE heap_trigger_func\(\) to stdout
        And psql should print heap_co_rule AS\n.*ON INSERT TO heap_table\n.*WHERE new\.column1 = 100 DO INSTEAD  INSERT INTO co_table_ex \(column1, column2, column3\) to stdout
        And psql should print \"heap_const_1\" FOREIGN KEY \(column1, column2, column3\) REFERENCES heap_table_ex\(column1, column2, column3\) to stdout
        And the user runs "psql -c '\d public.co_index_table' testdb"
        And psql should not print bitmap_co_index to stdout
        And the user runs "psql -c '\d public.heap_table_ex' testdb"
        And psql should not print heap_ex_trigger to stdout
        And verify that the data of "6" tables in "testdb" is validated after restore

    Scenario: Full Backup and Restore dropped database filtering tables with post data objects using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "heap" table "public.heap_table2" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "public.co_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_index_table" with index "ao_index" compression "None" in "testdb" with data
        And there is a "co" table "public.co_index_table" with index "co_index" compression "None" in "testdb" with data
        And there is a trigger "heap_trigger" on table "public.heap_table" in "testdb" based on function "heap_trigger_func"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table2 ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY heap_table ADD CONSTRAINT heap_const_1 FOREIGN KEY (column1, column2, column3) REFERENCES heap_table2(column1, column2, column3);' testdb"
        And the user runs "psql -c """create rule heap_ao_rule as on insert to heap_table where column1=100 do instead insert into ao_table values(27, 'restore', '2013-08-19');""" testdb"
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And there is a file "restore_file" with tables "public.ao_table,public.ao_index_table,public.heap_table,public.heap_table2"
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a trigger function "heap_trigger_func" on table "public.heap_table" in "testdb"
        When the user runs gpdbrestore with the stored timestamp and options "--table-file restore_file --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table2" in "testdb" with data
        And verify that there is no table "public.co_table_ex" in "testdb"
        And verify that there is no table "public.co_index_table" in "testdb"
        And the user runs "psql -c '\d public.ao_index_table' testdb"
        And psql should print \"bitmap_ao_index\" bitmap \(column3\) to stdout
        And psql should print \"btree_ao_index\" btree \(column1\) to stdout
        And the user runs "psql -c '\d public.heap_table' testdb"
        And psql should print \"heap_table_pkey\" PRIMARY KEY, btree \(column1, column2, column3\) to stdout
        And psql should print heap_trigger AFTER INSERT OR DELETE OR UPDATE ON heap_table FOR EACH STATEMENT EXECUTE PROCEDURE heap_trigger_func\(\) to stdout
        And psql should print heap_ao_rule AS\n.*ON INSERT TO heap_table\n.*WHERE new\.column1 = 100 DO INSTEAD  INSERT INTO ao_table \(column1, column2, column3\) to stdout
        And psql should print \"heap_const_1\" FOREIGN KEY \(column1, column2, column3\) REFERENCES heap_table2\(column1, column2, column3\) to stdout
        And verify that the data of "4" tables in "testdb" is validated after restore

    Scenario: Full Backup and Restore filtering tables post data objects with -C using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "heap" table "public.heap_table" with compression "None" in "testdb" with data
        And there is a "heap" table "public.heap_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_table" with compression "None" in "testdb" with data
        And there is a "co" table "public.co_table_ex" with compression "None" in "testdb" with data
        And there is a "ao" table "public.ao_index_table" with index "ao_index" compression "None" in "testdb" with data
        And there is a "co" table "public.co_index_table" with index "co_index" compression "None" in "testdb" with data
        And there is a trigger "heap_trigger" on table "public.heap_table" in "testdb" based on function "heap_trigger_func"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_table_ex ADD CONSTRAINT heap_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -c 'ALTER TABLE ONLY heap_table ADD CONSTRAINT heap_const_1 FOREIGN KEY (column1, column2, column3) REFERENCES heap_table_ex(column1, column2, column3);' testdb"
        And the user runs "psql -c """create rule heap_co_rule as on insert to heap_table where column1=100 do instead insert into co_table_ex values(27, 'restore', '2013-08-19');""" testdb"
        When the user runs "gpcrondump -a -x testdb -C --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the index "bitmap_co_index" in "testdb" is dropped
        And the index "bitmap_ao_index" in "testdb" is dropped
        And the user runs "psql -c 'CREATE INDEX bitmap_ao_index_new ON public.ao_index_table USING bitmap(column3);' testdb"
        Then there is a file "restore_file" with tables "public.ao_table,public.ao_index_table,public.heap_table"
        When the user runs gpdbrestore with the stored timestamp and options "--table-file restore_file --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "heap" table "public.heap_table_ex" in "testdb" with data
        And verify that there is a "co" table "public.co_table_ex" in "testdb" with data
        And verify that there is a "co" table "public.co_index_table" in "testdb" with data
        And the user runs "psql -c '\d public.ao_index_table' testdb"
        And psql should print \"bitmap_ao_index\" bitmap \(column3\) to stdout
        And psql should print \"btree_ao_index\" btree \(column1\) to stdout
        And psql should not print bitmap_ao_index_new to stdout
        And the user runs "psql -c '\d public.heap_table' testdb"
        And psql should print \"heap_table_pkey\" PRIMARY KEY, btree \(column1, column2, column3\) to stdout
        And psql should print heap_trigger AFTER INSERT OR DELETE OR UPDATE ON heap_table FOR EACH STATEMENT EXECUTE PROCEDURE heap_trigger_func\(\) to stdout
        And psql should print heap_co_rule AS\n.*ON INSERT TO heap_table\n.*WHERE new\.column1 = 100 DO INSTEAD  INSERT INTO co_table_ex \(column1, column2, column3\) to stdout
        And psql should print \"heap_const_1\" FOREIGN KEY \(column1, column2, column3\) REFERENCES heap_table_ex\(column1, column2, column3\) to stdout
        And the user runs "psql -c '\d public.co_index_table' testdb"
        And psql should not print bitmap_co_index to stdout
        And verify that the data of "6" tables in "testdb" is validated after restore

    Scenario: Full backup and restore for table names with multibyte (chinese) characters using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/create_multi_byte_char_table_name.sql testdb"
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/select_multi_byte_char.sql testdb"
        Then psql should print 1000 to stdout

    Scenario: Full Backup with option -T and Restore with exactly 1000 partitions using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "ao" table "ao_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_part_table" in "testdb" having "1000" partitions
        When the user runs "gpcrondump -a -x testdb -T public.ao_part_table --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "1" tables in "testdb" is validated after restore

    Scenario: Single table restore with shared sequence across multiple tables using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "seqtestdb" does not exist
        And database "seqtestdb" exists
        And there is a sequence "shared_seq" in "seqtestdb"
        And the user runs "psql -c """CREATE TABLE table1 (column1 INT4 DEFAULT nextval('shared_seq') NOT NULL, price NUMERIC);""" seqtestdb"
        And the user runs "psql -c """CREATE TABLE table2 (column1 INT4 DEFAULT nextval('shared_seq') NOT NULL, price NUMERIC);""" seqtestdb"
        And the user runs "psql -c 'CREATE TABLE table3 (column1 INT4)' seqtestdb"
        And the user runs "psql -c 'CREATE TABLE table4 (column1 INT4)' seqtestdb"
        And the user runs "psql -c 'INSERT INTO table1 (price) SELECT * FROM generate_series(1000,1009)' seqtestdb"
        And the user runs "psql -c 'INSERT INTO table2 (price) SELECT * FROM generate_series(2000,2009)' seqtestdb"
        And the user runs "psql -c 'INSERT INTO table3 SELECT * FROM generate_series(3000,3009)' seqtestdb"
        And the user runs "psql -c 'INSERT INTO table4 SELECT * FROM generate_series(4000,4009)' seqtestdb"
        When the user runs "gpcrondump -x seqtestdb -a --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "seqtestdb" is saved for verification
        And the user runs "psql -c 'INSERT INTO table2 (price) SELECT * FROM generate_series(2010,2019)' seqtestdb"
        When table "public.table1" is dropped in "seqtestdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.table1 --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.table1" in "seqtestdb" with data
        And the user runs "psql -c 'INSERT INTO table2 (price) SELECT * FROM generate_series(2020,2029)' seqtestdb"
        And verify that there are no duplicates in column "column1" of table "public.table2" in "seqtestdb"

    Scenario: Single table backup of the table with partitions in a separate schema using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "schema_parent, schema_child" exists in "testdb"
        And there is a "ao" partition table "schema_parent.ao_part_table" with compression "None" in "testdb" with data
        And partition "1" of partition table "schema_parent.ao_part_table" is assumed to be in schema "schema_child" in database "testdb"
        And all the data from "testdb" is saved for verification
        When the user runs "gpcrondump -a -v -x testdb -t schema_parent.ao_part_table" using netbackup
        Then gpcrondump should return a return code of 0

    Scenario: Database backup, while one of the tables have a partition in a separate schema using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "schema_parent, schema_child" exists in "testdb"
        And there is a "ao" partition table "schema_parent.ao_part_table" with compression "None" in "testdb" with data
        And there is a mixed storage partition table "public.part_mixed_1" in "testdb" with data
        And there is a "heap" table "schema_parent.heap_table" with compression "None" in "testdb" with data
        And partition "1" of partition table "schema_parent.ao_part_table" is assumed to be in schema "schema_child" in database "testdb"
        And all the data from "testdb" is saved for verification
        When the user runs "gpcrondump -a -v -x testdb -t schema_parent.ao_part_table" using netbackup
        Then gpcrondump should return a return code of 0

    Scenario: Database backup, while one of the tables have all the partitions in a separate schema using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "schema_parent, schema_child" exists in "testdb"
        And there is a "ao" partition table "schema_child.ao_part_table" with compression "None" in "testdb" with data
        And the user runs "psql -d testdb -c 'alter table schema_child.ao_part_table set schema schema_parent'"
        And there is a mixed storage partition table "public.part_mixed_1" in "testdb" with data
        And there is a "heap" table "schema_parent.heap_table" with compression "None" in "testdb" with data
        And all the data from "testdb" is saved for verification
        When the user runs "gpcrondump -a -v -x testdb -t schema_parent.ao_part_table" using netbackup
        Then gpcrondump should return a return code of 0

    Scenario: Full backup should not backup temp tables while using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is schema "a" exists in "testdb"
        And there is a "heap" table "a.heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "a.ao_part_table" with compression "None" in "testdb" with data
        And there is schema "b" exists in "testdb"
        And there is a "heap" table "b.heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "b.ao_part_table" with compression "None" in "testdb" with data
        And there is schema "z" exists in "testdb"
        And there is a "heap" table "z.heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "z.ao_part_table" with compression "None" in "testdb" with data
        And there is a "ao" table "ao_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "public.ao_part_table" with compression "None" in "testdb" with data
        And the user runs the query "CREATE TEMP TABLE temp_1 as select generate_series(1, 100);" on "testdb" for "120" seconds
        When the user runs "gpcrondump -x testdb -a --netbackup-block-size 4096" using netbackup
        And gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "40" tables in "testdb" is validated after restore

    Scenario: Full backup and restore using gpcrondump with pg_class lock using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data and 1000000 rows
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        When the user runs the "gpcrondump -a -x fullbkdb" in a worker pool "w1" using netbackup
        And this test sleeps for "2" seconds
        And the "gpcrondump" has a lock on the pg_class table in "fullbkdb"
        And the worker pool "w1" is cleaned up
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data

    Scenario: Full Backup and Restore using gp_dump without no-lock using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data and 1000000 rows
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "fullbkdb" exists for validation
        When the user runs the "gp_dump --gp-d=db_dumps --gp-s=p --gp-c fullbkdb --netbackup-block-size 4096" in a worker pool "w1" using netbackup
        And this test sleeps for "1" seconds
        And the "gp_dump" has a lock on the pg_class table in "fullbkdb"
        And the worker pool "w1" is cleaned up
        Then gp_dump should return a return code of 0
        And the timestamp from gp_dump is stored and subdir is " "
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        When the user runs gp_restore with the the stored timestamp and subdir in "fullbkdb" using netbackup
        Then gp_restore should return a return code of 0
        And verify that there is a "heap" table "heap_table" in "fullbkdb" with data
        And verify that there is a "ao" table "ao_part_table" in "fullbkdb" with data

    Scenario: Restore -T for full dump should restore metadata/postdata objects for tablenames with English and multibyte (chinese) characters using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And there is a "ao" table "public.ao_index_table" with index "ao_index" compression "None" in "testdb" with data
        And there is a "co" table "public.co_index_table" with index "co_index" compression "None" in "testdb" with data
        And there is a "heap" table "public.heap_index_table" with index "heap_index" compression "None" in "testdb" with data
        And the user runs "psql -c 'ALTER TABLE ONLY public.heap_index_table ADD CONSTRAINT heap_index_table_pkey PRIMARY KEY (column1, column2, column3);' testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/create_multi_byte_char_tables.sql testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/primary_key_multi_byte_char_table_name.sql testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/index_multi_byte_char_table_name.sql testdb"
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/describe_multi_byte_char.sql testdb > /tmp/describe_multi_byte_char_before"
        And the user runs "psql -c '\d public.ao_index_table' testdb > /tmp/describe_ao_index_table_before"
        When there is a backupfile of tables "ao_index_table, co_index_table, heap_index_table" in "testdb" exists for validation
        And table "public.ao_index_table" is dropped in "testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/drop_table_with_multi_byte_char.sql testdb"
        When the user runs gpdbrestore with the stored timestamp and options "--table-file gppylib/test/behave/mgmt_utils/steps/data/include_tables_with_metadata_postdata --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/select_multi_byte_char_tables.sql testdb"
        Then psql should print 1000 to stdout 4 times
        And verify that there is a "ao" table "ao_index_table" in "testdb" with data
        And verify that there is a "co" table "co_index_table" in "testdb" with data
        And verify that there is a "heap" table "heap_index_table" in "testdb" with data
        When the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/describe_multi_byte_char.sql testdb > /tmp/describe_multi_byte_char_after"
        And the user runs "psql -c '\d public.ao_index_table' testdb > /tmp/describe_ao_index_table_after"
        Then verify that the contents of the files "/tmp/describe_multi_byte_char_before" and "/tmp/describe_multi_byte_char_after" are identical
        And verify that the contents of the files "/tmp/describe_ao_index_table_before" and "/tmp/describe_ao_index_table_after" are identical
        And the file "/tmp/describe_multi_byte_char_before" is removed from the system
        And the file "/tmp/describe_multi_byte_char_after" is removed from the system
        And the file "/tmp/describe_ao_index_table_before" is removed from the system
        And the file "/tmp/describe_ao_index_table_after" is removed from the system

    Scenario: Restore -T for full dump should restore GRANT privileges for tablenames with English and multibyte (chinese) characters using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/create_multi_byte_char_tables.sql testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/primary_key_multi_byte_char_table_name.sql testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/index_multi_byte_char_table_name.sql testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/grant_multi_byte_char_table_name.sql testdb"
        And the user runs """psql -c "CREATE ROLE test_gpadmin LOGIN ENCRYPTED PASSWORD 'changeme' SUPERUSER INHERIT CREATEDB CREATEROLE RESOURCE QUEUE pg_default;" testdb"""
        And the user runs """psql -c "CREATE ROLE customer LOGIN ENCRYPTED PASSWORD 'changeme' NOSUPERUSER INHERIT NOCREATEDB NOCREATEROLE RESOURCE QUEUE pg_default;" testdb"""
        And the user runs """psql -c "CREATE ROLE select_group NOSUPERUSER INHERIT NOCREATEDB NOCREATEROLE RESOURCE QUEUE pg_default;" testdb"""
        And the user runs """psql -c "CREATE ROLE test_group NOSUPERUSER INHERIT NOCREATEDB NOCREATEROLE RESOURCE QUEUE pg_default;" testdb"""
        And the user runs """psql -c "CREATE SCHEMA customer AUTHORIZATION test_gpadmin" testdb"""
        And the user runs """psql -c "GRANT ALL ON SCHEMA customer TO test_gpadmin;" testdb"""
        And the user runs """psql -c "GRANT ALL ON SCHEMA customer TO customer;" testdb"""
        And the user runs """psql -c "GRANT USAGE ON SCHEMA customer TO select_group;" testdb"""
        And the user runs """psql -c "GRANT ALL ON SCHEMA customer TO test_group;" testdb"""
        And there is a "heap" table "customer.heap_index_table_1" with index "heap_index_1" compression "None" in "testdb" with data
        And the user runs """psql -c "ALTER TABLE customer.heap_index_table_1 owner to customer" testdb"""
        And the user runs """psql -c "GRANT ALL ON TABLE customer.heap_index_table_1 TO customer;" testdb"""
        And the user runs """psql -c "GRANT ALL ON TABLE customer.heap_index_table_1 TO test_group;" testdb"""
        And the user runs """psql -c "GRANT SELECT ON TABLE customer.heap_index_table_1 TO select_group;" testdb"""
        And there is a "heap" table "customer.heap_index_table_2" with index "heap_index_2" compression "None" in "testdb" with data
        And the user runs """psql -c "ALTER TABLE customer.heap_index_table_2 owner to customer" testdb"""
        And the user runs """psql -c "GRANT ALL ON TABLE customer.heap_index_table_2 TO customer;" testdb"""
        And the user runs """psql -c "GRANT SELECT, UPDATE, INSERT, DELETE ON TABLE customer.heap_index_table_2 TO test_group;" testdb"""
        And the user runs """psql -c "GRANT SELECT ON TABLE customer.heap_index_table_2 TO select_group;" testdb"""
        And there is a "heap" table "customer.heap_index_table_3" with index "heap_index_3" compression "None" in "testdb" with data
        And the user runs """psql -c "ALTER TABLE customer.heap_index_table_3 owner to customer" testdb"""
        And the user runs """psql -c "GRANT ALL ON TABLE customer.heap_index_table_3 TO customer;" testdb"""
        And the user runs """psql -c "GRANT SELECT, UPDATE, INSERT, DELETE ON TABLE customer.heap_index_table_3 TO test_group;" testdb"""
        And the user runs """psql -c "GRANT SELECT ON TABLE customer.heap_index_table_3 TO select_group;" testdb"""
        And the user runs """psql -c "ALTER ROLE customer SET search_path = customer, public;" testdb"""
        When the user runs "psql -c '\d customer.heap_index_table_1' testdb > /tmp/describe_heap_index_table_1_before"
        And the user runs "psql -c '\dp customer.heap_index_table_1' testdb > /tmp/privileges_heap_index_table_1_before"
        And the user runs "psql -c '\d customer.heap_index_table_2' testdb > /tmp/describe_heap_index_table_2_before"
        And the user runs "psql -c '\dp customer.heap_index_table_2' testdb > /tmp/privileges_heap_index_table_2_before"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/describe_multi_byte_char.sql testdb > /tmp/describe_multi_byte_char_before"
        When the user runs "gpcrondump -x testdb -g -G -a -b -v -u /tmp --rsyncable --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When there is a backupfile of tables "customer.heap_index_table_1, customer.heap_index_table_2, customer.heap_index_table_3" in "testdb" exists for validation
        And table "customer.heap_index_table_1" is dropped in "testdb"
        And table "customer.heap_index_table_2" is dropped in "testdb"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/drop_table_with_multi_byte_char.sql testdb"
        And the user runs gpdbrestore with the stored timestamp and options "--table-file gppylib/test/behave/mgmt_utils/steps/data/include_tables_with_grant_permissions -u /tmp --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        When the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/select_multi_byte_char_tables.sql testdb"
        Then psql should print 1000 to stdout 4 times
        And verify that there is a "heap" table "customer.heap_index_table_1" in "testdb" with data
        And verify that there is a "heap" table "customer.heap_index_table_2" in "testdb" with data
        And verify that there is a "heap" table "customer.heap_index_table_3" in "testdb" with data
        When the user runs "psql -c '\d customer.heap_index_table_1' testdb > /tmp/describe_heap_index_table_1_after"
        And the user runs "psql -c '\dp customer.heap_index_table_1' testdb > /tmp/privileges_heap_index_table_1_after"
        And the user runs "psql -c '\d customer.heap_index_table_2' testdb > /tmp/describe_heap_index_table_2_after"
        And the user runs "psql -c '\dp customer.heap_index_table_2' testdb > /tmp/privileges_heap_index_table_2_after"
        And the user runs "psql -f gppylib/test/behave/mgmt_utils/steps/data/describe_multi_byte_char.sql testdb > /tmp/describe_multi_byte_char_after"
        Then verify that the contents of the files "/tmp/describe_heap_index_table_1_before" and "/tmp/describe_heap_index_table_1_after" are identical
        And verify that the contents of the files "/tmp/describe_heap_index_table_2_before" and "/tmp/describe_heap_index_table_2_after" are identical
        And verify that the contents of the files "/tmp/privileges_heap_index_table_1_before" and "/tmp/privileges_heap_index_table_1_after" are identical
        And verify that the contents of the files "/tmp/privileges_heap_index_table_2_before" and "/tmp/privileges_heap_index_table_2_after" are identical
        And verify that the contents of the files "/tmp/describe_multi_byte_char_before" and "/tmp/describe_multi_byte_char_after" are identical
        And the file "/tmp/describe_heap_index_table_1_before" is removed from the system
        And the file "/tmp/describe_heap_index_table_1_after" is removed from the system
        And the file "/tmp/privileges_heap_index_table_1_before" is removed from the system
        And the file "/tmp/privileges_heap_index_table_1_after" is removed from the system
        And the file "/tmp/describe_heap_index_table_2_before" is removed from the system
        And the file "/tmp/describe_heap_index_table_2_after" is removed from the system
        And the file "/tmp/privileges_heap_index_table_2_before" is removed from the system
        And the file "/tmp/privileges_heap_index_table_2_after" is removed from the system
        And the file "/tmp/describe_multi_byte_char_before" is removed from the system
        And the file "/tmp/describe_multi_byte_char_after" is removed from the system

    Scenario: Redirected Restore (RR) Full Backup and Restore without -e option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And the database "testdb" does not exist
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And all the data from "fullbkdb" is saved for verification
        When the user runs "gpcrondump -x fullbkdb -a" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--redirect=testdb" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_part_table" in "testdb" with data

    Scenario: (RR) Full Backup and Restore with -e option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And the database "testdb" does not exist
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And all the data from "fullbkdb" is saved for verification
        When the user runs "gpcrondump -x fullbkdb -a --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--redirect=testdb --netbackup-block-size 1024" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_part_table" in "testdb" with data

    Scenario: (RR) Full backup and restore with -T using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "fullbkdb" does not exist
        And database "fullbkdb" exists
        And the database "testdb" does not exist
        And there is a "heap" table "heap_table" with compression "None" in "fullbkdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "fullbkdb" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "fullbkdb" with data
        When the user runs "gpcrondump -a -x fullbkdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "fullbkdb" is saved for verification
        When the user truncates "public.ao_index_table" tables in "fullbkdb"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.ao_index_table --redirect=testdb --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "ao" table "public.ao_index_table" in "testdb" with data

    Scenario: (RR) Full Backup and Restore with --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And the database "testdb1" does not exist
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "testdb" exists for validation
        When the user runs "gpcrondump -a -x testdb --prefix=foo --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --redirect=testdb1 --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And there should be dump files under " " with prefix "foo"
        And verify that there is a "heap" table "heap_table" in "testdb1" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb1" with data

    Scenario: (RR) Full Backup and Restore with --prefix option for multiple databases using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "testdb" does not exist
        And database "testdb" exists
        And the database "testdb1" does not exist
        And database "testdb1" exists
        And the database "testdb2" does not exist
        And database "testdb2" exists
        And there is a "heap" table "heap_table1" with compression "None" in "testdb1" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb1" with data
        And there is a backupfile of tables "heap_table1, ao_part_table" in "testdb1" exists for validation
        And there is a "heap" table "heap_table2" with compression "None" in "testdb2" with data
        And there is a backupfile of tables "heap_table2" in "testdb2" exists for validation
        When the user runs "gpcrondump -a -x testdb1,testdb2 --prefix=foo --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --redirect=testdb --netbackup-block-size 4096" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And there should be dump files under " " with prefix "foo"
        And verify that there is a "heap" table "heap_table1" in "testdb" with data
        And verify that there is a "ao" table "ao_part_table" in "testdb" with data

    Scenario: Full Backup and Restore with the master dump file missing using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -x testdb -a --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbrestore should print Backup for given timestamp was performed using NetBackup. Querying NetBackup server to check for the dump file. to stdout

    Scenario: Full Backup and Restore with the master dump file missing without compression using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -x testdb  -z -a --netbackup-block-size 4096" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbrestore should print Backup for given timestamp was performed using NetBackup. Querying NetBackup server to check for the dump file. to stdout

    Scenario: Uppercase Dbname (UD) Full Backup and Restore using timestamp using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "TESTING" does not exist
        And database "TESTING" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "TESTING" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "TESTING" with data
        And all the data from "TESTING" is saved for verification
        When the user runs "gpcrondump -x TESTING -a --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbestore should not print Issue with analyze of to stdout
        And verify that there is a "heap" table "public.heap_table" in "TESTING" with data
        And verify that there is a "ao" table "public.ao_part_table" in "TESTING" with data

    Scenario: (UD) Full backup and restore with -T using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "TESTING" does not exist
        And database "TESTING" exists
        And there is a "heap" table "heap_table" with compression "None" in "TESTING" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "TESTING" with data
        And there is a "ao" table "ao_index_table" with compression "None" in "TESTING" with data
        When the user runs "gpcrondump -a -x TESTING --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "TESTING" is saved for verification
        When the user truncates "public.ao_index_table" tables in "TESTING"
        And the user runs gpdbrestore with the stored timestamp and options "-T public.ao_index_table --netbackup-block-size 1024" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbestore should not print Issue with analyze of to stdout
        And verify that there is a "ao" table "public.ao_index_table" in "TESTING" with data

    Scenario: (UD) Full Backup and Restore with --prefix option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the prefix "foo" is stored
        And there are no backup files
        And the database "TESTING" does not exist
        And database "TESTING" exists
        And there is a "heap" table "heap_table" with compression "None" in "TESTING" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "TESTING" with data
        And there is a backupfile of tables "heap_table, ao_part_table" in "TESTING" exists for validation
        When the user runs "gpcrondump -a -x TESTING --prefix=foo --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--prefix=foo --netbackup-block-size 1024" using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbestore should not print Issue with analyze of to stdout
        And there should be dump files under " " with prefix "foo"
        And verify that there is a "heap" table "heap_table" in "TESTING" with data
        And verify that there is a "ao" table "ao_part_table" in "TESTING" with data

    Scenario: Full backup and Restore should create the gp_toolkit schema with -e option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And all the data from "testdb" is saved for verification
        And the gp_toolkit schema for "testdb" is saved for verification
        When the user runs "gpcrondump -x testdb -a --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "10" tables in "testdb" is validated after restore
        And the gp_toolkit schema for "testdb" is verified after restore

    Scenario: Redirected Restore should create the gp_toolkit schema with -e option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And all the data from "testdb" is saved for verification
        And the gp_toolkit schema for "testdb" is saved for verification
        When the user runs "gpcrondump -x testdb -a --netbackup-block-size 1024" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--redirect=fullbkdb --netbackup-block-size 1024" using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "10" tables in "fullbkdb" is validated after restore
        And the gp_toolkit schema for "fullbkdb" is verified after restore

    Scenario: Redirected Restore should create the gp_toolkit schema without -e option using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And the database "fullbkdb" does not exist
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And all the data from "testdb" is saved for verification
        And the gp_toolkit schema for "testdb" is saved for verification
        When the user runs "gpcrondump -x testdb -a --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        When the user runs gpdbrestore with the stored timestamp and options "--redirect=fullbkdb --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that the data of "10" tables in "fullbkdb" is validated after restore
        And the gp_toolkit schema for "fullbkdb" is verified after restore

    Scenario: gpdbrestore with noanalyze using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        And the database "testdb" does not exist
        And database "testdb" exists
        When the user runs gpdbrestore with the stored timestamp and options "--noanalyze -a --netbackup-block-size 2048" without -e option using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbestore should print Analyze bypassed on request to stdout
        And verify that the data of "10" tables in "testdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb"

    Scenario: gpdbrestore without noanalyze using NetBackup
        Given the database is running
        And the netbackup params have been parsed
        And there are no backup files
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        When the user runs "gpcrondump -a -x testdb --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And all the data from "testdb" is saved for verification
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And gpdbestore should print Commencing analyze of testdb database to stdout
        And gpdbestore should print Analyze of testdb completed without error to stdout
        And verify that the data of "10" tables in "testdb" is validated after restore
        And verify that the tuple count of all appendonly tables are consistent in "testdb"

    @nbusmoke
    Scenario: Verify that metadata files get backed up to NetBackup server
        Given the database is running
        And the netbackup params have been parsed
        And the database "testdb" does not exist
        And database "testdb" exists
        And there are no backup files
        And there are no report files in "master_data_directory"
        And there are no status files in "segment_data_directory"
        And there is a "heap" table "heap_table" with compression "None" in "testdb" with data
        And there is a "ao" partition table "ao_part_table" with compression "quicklz" in "testdb" with data
        And all the data from "testdb" is saved for verification
        When the user runs "gpcrondump -x testdb -a -g -G --netbackup-block-size 2048" using netbackup
        Then gpcrondump should return a return code of 0
        And the timestamp from gpcrondump is stored
        And verify that report file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that cdatabase file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that state file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that config file with prefix " " under subdir " " has been backed up using NetBackup
        And verify that global file with prefix " " under subdir " " has been backed up using NetBackup
        When the user runs gpdbrestore with the stored timestamp using netbackup
        Then gpdbrestore should return a return code of 0
        And verify that there is a "heap" table "public.heap_table" in "testdb" with data
        And verify that there is a "ao" table "public.ao_part_table" in "testdb" with data
