#  pg_config --includedir-server
#include_dir := /Users/cwelton/greenplum-db-devel/include/postgresql/server
include_dir := /Users/cwelton/cdb2/cdb-pg/src/include

all:
	cc -I. -I$(include_dir) -g -c mapred.c
	cc -I. -I$(include_dir) -g -c words.c
	cc -I. -I$(include_dir) -g -c words2.c
	cc -bundle -flat_namespace -undefined suppress -o mapred.so mapred.o words.o words2.o