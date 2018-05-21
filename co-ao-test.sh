#!/bin/bash

pushd ./src/test/isolation2/
./pg_isolation2_regress create-loaded-cluster

popd

primary1=`psql postgres -t -c "select b.datadir || '/base/' || d.oid from gp_segment_configuration b right join pg_database d on true where d.datname='isolation2test' and b.content = 0 and role = 'p';"`
primary2=`psql postgres -t -c "select b.datadir || '/base/' || d.oid from gp_segment_configuration b right join pg_database d on true where d.datname='isolation2test' and b.content = 1 and role = 'p';"`
primary3=`psql postgres -t -c "select b.datadir || '/base/' || d.oid from gp_segment_configuration b right join pg_database d on true where d.datname='isolation2test' and b.content = 2 and role = 'p';"`

for ((j=1; j<$1; j++)) do
    touch $primary1/testfile.$j;
    touch $primary2/testfile.$j;
    touch $primary3/testfile.$j;
done

time psql isolation2test -c "DROP TABLE co_ao_test_5;"
time psql isolation2test -c "DROP TABLE co_ao_test_1600;"
