
\c template1;
drop database if exists gpperfmon;
create database gpperfmon;
\c gpperfmon;


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  metrics 
--

create table metrics_history (
       ctime timestamp(0) not null, -- record creation time
       hostname varchar(64) not null, -- hostname of system this metric belongs to
       mem_total bigint not null, mem_used bigint not null, -- total system memory
       mem_actual_used bigint not null, mem_actual_free bigint not null, -- memory used
       swap_total bigint not null, swap_used bigint not null, -- total swap space
       swap_page_in bigint not null, swap_page_out bigint not null, -- swap pages in
       cpu_user float not null, cpu_sys float not null, cpu_idle float not null, -- cpu usage
       load0 float not null, load1 float not null, load2 float not null, -- cpu load avgs
       quantum int not null, -- interval between metric collection for this entry
       disk_ro_rate bigint not null, -- system disk read ops per second
       disk_wo_rate bigint not null, -- system disk write ops per second
       disk_rb_rate bigint not null, -- system disk read bytes per second
       disk_wb_rate bigint not null, -- system disk write bytes per second
       net_rp_rate bigint not null,  -- system net read packets per second
       net_wp_rate bigint not null,  -- system net write packets per second
       net_rb_rate bigint not null,  -- system net read bytes per second
       net_wb_rate bigint not null   -- system net write bytes per second
) 
with (fillfactor=100)
distributed by (ctime)
partition by range (ctime)(start (date '2008-07-01') end (date '2010-01-01') EVERY (interval '1 month'));

create external web table metrics_now (
       like metrics_history
) execute 'cat gpperfmon/data/metrics_now.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table metrics_tail (
       like metrics_history
) execute 'cat gpperfmon/data/metrics_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table _metrics_tail (
        like metrics_history
) execute 'cat gpperfmon/data/_metrics_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  queries
--

create table queries_history (
       ctime timestamp(0), -- record creation time
       tmid int not null,  -- time id
       ssid int not null,    -- session id
       ccnt int not null,    -- command count in session
       uname varchar(64) not null, -- username that issued the query
       db varchar(64) not null, -- database name for the query
       cost int not null, -- query cost (not implemented)
       tsubmit timestamp(0) not null, -- query submit time
       tstart timestamp(0) not null,  -- query start time
       tfin timestamp(0) not null,    -- query end time
       status varchar(64) not null,   -- query status (start, end, abort)
       rows_out bigint not null, -- rows out for query
       cpu_elapsed bigint not null, -- cpu usage for query across all segments
       cpu_currpct float not null, -- current cpu percent avg for all processes executing query
       skew_cpu float not null,    -- coefficient of variance for cpu_elapsed of iterators across segments for query
       skew_rows float not null,   -- coefficient of variance for rows_in of iterators across segments for query
       query_hash bigint not null, -- (not implemented)
       query_text text not null default '', -- query text
       query_plan text not null default ''  -- query plan (not implemented)
)
with (fillfactor=100)
distributed by (ctime)
partition by range (ctime)(start (date '2008-07-01') end (date '2010-01-01') EVERY (interval '1 month'));




create external web table queries_now (
        like queries_history
) execute 'python $GPHOME/bin/lib/gpmon_catqrynow.py 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');

create external web table queries_now_fast (
       ctime timestamp(0),
       tmid int,
       ssid int,    -- gp_session_id
       ccnt int,    -- gp_command_count
       uname varchar(64),
       db varchar(64),
       cost int,
       tsubmit timestamp(0), 
       tstart timestamp(0), 
       tfin timestamp(0),
       status varchar(64),
       rows_out bigint,
       cpu_elapsed bigint,
       cpu_currpct float,
       skew_cpu float,		-- always 0
       skew_rows float
       -- excluded: query_text text
       -- excluded: query_plan text
) execute 'cat gpperfmon/data/queries_now.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');

create external web table queries_tail (
        like queries_history
) execute 'cat gpperfmon/data/queries_tail.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');


create external web table _queries_tail (
        like queries_history
) execute 'cat gpperfmon/data/_queries_tail.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  query iterators
--

create table iterators_history (
       ctime timestamp(0), -- record creation time
       tmid int not null,  -- time id
       ssid int not null,    -- session id
       ccnt int not null,    -- command count for session
       segid int not null,   -- segment id
       pid int not null, -- pid of process executing iterator
       nid int not null, -- node id
       pnid int not null,-- parent node id
       hostname varchar(64) not null,
       ntype varchar(64) not null, -- node type
       nstatus varchar(64) not null, -- node status
       tstart timestamp(0) not null, -- time started
       tduration int not null, -- duration of execution
       pmemsize bigint not null, -- TODO: fill in these
       pmemmax bigint not null,
       memsize bigint not null, 
       memresid bigint not null,
       memshare bigint not null,
       pagefaults bigint not null, -- process page faults 
       pagefaults_elapsed bigint not null, -- total process page faults
       cpu_elapsed bigint not null, -- process cpu usage total
       cpu_currpct float not null, -- process current cpu percentage
       phase varchar(64) not null, -- iterator phase
       rowsout bigint not null default 0, -- rows out
       rowsout_est bigint not null default 0, -- planner estimate of rows out
       m0_name varchar(64) not null default '', -- iterator metric name
       m0_unit varchar(64) not null default '', -- iterator metric unit (rows, bytes, etc.)
       m0_val bigint not null default 0,         -- current metric value
       m0_est bigint not null default 0,         -- estimated final value
       m1_name varchar(64) not null default '',
       m1_unit varchar(64) not null default '',
       m1_val bigint not null default 0,
       m1_est bigint not null default 0,
       m2_name varchar(64) not null default '',
       m2_unit varchar(64) not null default '',
       m2_val bigint not null default 0,
       m2_est bigint not null default 0,
       m3_name varchar(64) not null default '',
       m3_unit varchar(64) not null default '',
       m3_val bigint not null default 0,
       m3_est bigint not null default 0,
       m4_name varchar(64) not null default '',
       m4_unit varchar(64) not null default '',
       m4_val bigint not null default 0,
       m4_est bigint not null default 0,
       m5_name varchar(64) not null default '',
       m5_unit varchar(64) not null default '',
       m5_val bigint not null default 0,
       m5_est bigint not null default 0,
       m6_name varchar(64) not null default '',
       m6_unit varchar(64) not null default '',
       m6_val bigint not null default 0,
       m6_est bigint not null default 0,
       m7_name varchar(64) not null default '',
       m7_unit varchar(64) not null default '',
       m7_val bigint not null default 0,
       m7_est bigint not null default 0,
       m8_name varchar(64) not null default '',
       m8_unit varchar(64) not null default '',
       m8_val bigint not null default 0,
       m8_est bigint not null default 0,
       m9_name varchar(64) not null default '',
       m9_unit varchar(64) not null default '',
       m9_val bigint not null default 0,
       m9_est bigint not null default 0,
       m10_name varchar(64) not null default '',
       m10_unit varchar(64) not null default '',
       m10_val bigint not null default 0,
       m10_est bigint not null default 0,
       m11_name varchar(64) not null default '',
       m11_unit varchar(64) not null default '',
       m11_val bigint not null default 0,
       m11_est bigint not null default 0,
       m12_name varchar(64) not null default '',
       m12_unit varchar(64) not null default '',
       m12_val bigint not null default 0,
       m12_est bigint not null default 0,
       m13_name varchar(64) not null default '',
       m13_unit varchar(64) not null default '',
       m13_val bigint not null default 0,
       m13_est bigint not null default 0,
       m14_name varchar(64) not null default '',
       m14_unit varchar(64) not null default '',
       m14_val bigint not null default 0,
       m14_est bigint not null default 0,
       m15_name varchar(64) not null default '',
       m15_unit varchar(64) not null default '',
       m15_val bigint not null default 0,
       m15_est bigint not null default 0
)
with (fillfactor=100)
distributed by (ctime)
partition by range (ctime)(start (date '2008-07-01') end (date '2010-01-01') EVERY (interval '1 month'));



create external web table iterators_now (
        like iterators_history
) execute 'cat gpperfmon/data/iterators_now.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table iterators_tail (
        like iterators_history
) execute 'cat gpperfmon/data/iterators_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table _iterators_tail (
        like iterators_history
) execute 'cat gpperfmon/data/_iterators_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');



create external web table master_data_dir (hostname text, dir text)
execute E'python -c "import socket, os; print socket.gethostname() + \\"|\\" + os.getcwd()"' on master
format 'csv' (delimiter '|');

revoke all on database gpperfmon from public;

-- for web ui auth everyone needs connect permissions
grant connect on database gpperfmon to public;
