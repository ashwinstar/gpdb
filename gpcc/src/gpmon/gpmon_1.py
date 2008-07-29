import sys, os
from datetime import *

print '''
\c template1;
drop database if exists gpmon_1;
create database gpmon_1;
\c gpmon_1;


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  metrics 
--

create table metrics_history (
       ctime timestamp(0) not null,
       hostname varchar(64) not null,
       mem_total bigint not null, mem_used bigint not null,
       mem_actual_used bigint not null, mem_actual_free bigint not null,
       swap_total bigint not null, swap_used bigint not null, 
       swap_page_in bigint not null, swap_page_out bigint not null,
       cpu_user float not null, cpu_sys float not null, cpu_idle float not null,
       load0 float not null, load1 float not null, load2 float not null,
       disk_ro bigint not null, disk_wo bigint not null, disk_rb bigint not null, disk_wb bigint not null, 
       net_rp bigint not null, net_wp bigint not null, net_rb bigint not null, net_wb bigint not null
) 
with (fillfactor=100)
distributed by (ctime);


create external web table metrics_now (
       ctime timestamp(0),
       hostname varchar(64),
       mem_total bigint, mem_used bigint,
       mem_actual_used bigint, mem_actual_free bigint,
       swap_total bigint, swap_used bigint, 
       swap_page_in bigint, swap_page_out bigint,
       cpu_user float, cpu_sys float, cpu_idle float,
       load0 float, load1 float, load2 float,
       disk_ro bigint, disk_wo bigint, disk_rb bigint, disk_wb bigint, 
       net_rp bigint, net_wp bigint, net_rb bigint, net_wb bigint
) execute 'cat gpmon.1/metrics_now.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table metrics_tail (
       ctime timestamp(0),
       hostname varchar(64),
       mem_total bigint, mem_used bigint,
       mem_actual_used bigint, mem_actual_free bigint,
       swap_total bigint, swap_used bigint, 
       swap_page_in bigint, swap_page_out bigint,
       cpu_user float, cpu_sys float, cpu_idle float,
       load0 float, load1 float, load2 float,
       disk_ro bigint, disk_wo bigint, disk_rb bigint, disk_wb bigint, 
       net_rp bigint, net_wp bigint, net_rb bigint, net_wb bigint
) execute 'cat gpmon.1/metrics_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table _metrics_tail (
       ctime timestamp(0),
       hostname varchar(64),
       mem_total bigint, mem_used bigint,
       mem_actual_used bigint, mem_actual_free bigint,
       swap_total bigint, swap_used bigint, 
       swap_page_in bigint, swap_page_out bigint,
       cpu_user float, cpu_sys float, cpu_idle float,
       load0 float, load1 float, load2 float,
       disk_ro bigint, disk_wo bigint, disk_rb bigint, disk_wb bigint, 
       net_rp bigint, net_wp bigint, net_rb bigint, net_wb bigint
) execute 'cat gpmon.1/_metrics_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  queries
--

create table queries_history (
       ctime timestamp(0),
       tmid int not null,
       xid int not null,
       cid int not null,
       -- ssid int not null,    -- for 3.2, add gp_session_id
       -- ccnt int not null,    -- for 3.2, add gp_command_count
       uname varchar(64) not null,
       db varchar(64) not null,
       cost int not null,
       tsubmit timestamp(0) not null, 
       tstart timestamp(0) not null, 
       tfin timestamp(0) not null,
       status varchar(64) not null,
       rows_out bigint not null,
       cpu_elapsed bigint not null,
       cpu_currpct float not null,
       skew_cpu float not null,
       skew_rows float not null,
       query_text text not null default '',
       query_plan text not null default ''
)
with (fillfactor=100)
distributed by (ctime);



create external web table queries_now (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
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
       skew_rows float,		-- always 0
       query_text text,
       query_plan text
) execute 'python $GPHOME/bin/lib/gpmon_catqrynow.py 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');

create external web table queries_now_fast (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
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
) execute 'cat gpmon.1/queries_now.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');

create external web table queries_tail (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
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
       skew_cpu float,
       skew_rows float,
       query_text text,
       query_plan text
) execute 'cat gpmon.1/queries_tail.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');


create external web table _queries_tail (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
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
       skew_cpu float,
       skew_rows float,
       query_text text,
       query_plan text
) execute 'cat gpmon.1/_queries_tail.dat 2> /dev/null || true' on master format 'csv' (delimiter '|' NULL as 'null');


--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  query iterators
--

create table iterators_history (
       ctime timestamp(0),
       tmid int not null,
       xid int not null,
       cid int not null,
       -- ssid int not null,    -- for 3.2, add gp_session_id
       -- ccnt int not null,    -- for 3.2, add gp_command_count
       segid int not null,
       pid int not null,
       nid int not null,
       pnid int not null,
       hostname varchar(64) not null,
       ntype varchar(64) not null,
       nstatus varchar(64) not null,
       tstart timestamp(0) not null,
       tduration int not null,
       pmemsize bigint not null,
       pmemmax bigint not null,
       memsize bigint not null,
       memresid bigint not null,
       memshare bigint not null,
       pfaults bigint not null,
       pfaults_elapsed bigint not null,
       cpu_elapsed bigint not null,
       cpu_currpct float not null,
       phase varchar(64) not null,
       m0_name varchar(64) not null default '',
       m0_unit varchar(64) not null default '',
       m0_in bigint not null default 0,
       m0_out bigint not null default 0,
       m1_name varchar(64) not null default '',
       m1_unit varchar(64) not null default '',
       m1_in bigint not null default 0,
       m1_out bigint not null default 0,
       m2_name varchar(64) not null default '',
       m2_unit varchar(64) not null default '',
       m2_in bigint not null default 0,
       m2_out bigint not null default 0,
       m3_name varchar(64) not null default '',
       m3_unit varchar(64) not null default '',
       m3_in bigint not null default 0,
       m3_out bigint not null default 0,
       m4_name varchar(64) not null default '',
       m4_unit varchar(64) not null default '',
       m4_in bigint not null default 0,
       m4_out bigint not null default 0,
       m5_name varchar(64) not null default '',
       m5_unit varchar(64) not null default '',
       m5_in bigint not null default 0,
       m5_out bigint not null default 0,
       m6_name varchar(64) not null default '',
       m6_unit varchar(64) not null default '',
       m6_in bigint not null default 0,
       m6_out bigint not null default 0,
       m7_name varchar(64) not null default '',
       m7_unit varchar(64) not null default '',
       m7_in bigint not null default 0,
       m7_out bigint not null default 0,
       m8_name varchar(64) not null default '',
       m8_unit varchar(64) not null default '',
       m8_in bigint not null default 0,
       m8_out bigint not null default 0,
       m9_name varchar(64) not null default '',
       m9_unit varchar(64) not null default '',
       m9_in bigint not null default 0,
       m9_out bigint not null default 0,
       m10_name varchar(64) not null default '',
       m10_unit varchar(64) not null default '',
       m10_in bigint not null default 0,
       m10_out bigint not null default 0,
       m11_name varchar(64) not null default '',
       m11_unit varchar(64) not null default '',
       m11_in bigint not null default 0,
       m11_out bigint not null default 0,
       m12_name varchar(64) not null default '',
       m12_unit varchar(64) not null default '',
       m12_in bigint not null default 0,
       m12_out bigint not null default 0,
       m13_name varchar(64) not null default '',
       m13_unit varchar(64) not null default '',
       m13_in bigint not null default 0,
       m13_out bigint not null default 0,
       m14_name varchar(64) not null default '',
       m14_unit varchar(64) not null default '',
       m14_in bigint not null default 0,
       m14_out bigint not null default 0,
       m15_name varchar(64) not null default '',
       m15_unit varchar(64) not null default '',
       m15_in bigint not null default 0,
       m15_out bigint not null default 0
)
with (fillfactor=100)
distributed by (ctime);



create external web table iterators_now (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
       segid int,
       pid int,
       nid int,
       pnid int,
       hostname varchar(64),
       ntype varchar(64),
       nstatus varchar(64),
       tstart timestamp(0),
       tduration int,
       pmemsize bigint,
       pmemmax bigint,
       memsize bigint,
       memresid bigint,
       memshare bigint,
       pfaults bigint,
       pfaults_elapsed bigint,
       cpu_elapsed bigint,
       cpu_currpct float,
       phase varchar(64),
       m0_name varchar(64),
       m0_unit varchar(64),
       m0_in bigint,
       m0_out bigint,
       m1_name varchar(64),
       m1_unit varchar(64),
       m1_in bigint,
       m1_out bigint,
       m2_name varchar(64),
       m2_unit varchar(64),
       m2_in bigint,
       m2_out bigint,
       m3_name varchar(64),
       m3_unit varchar(64),
       m3_in bigint,
       m3_out bigint,
       m4_name varchar(64),
       m4_unit varchar(64),
       m4_in bigint,
       m4_out bigint,
       m5_name varchar(64),
       m5_unit varchar(64),
       m5_in bigint,
       m5_out bigint,
       m6_name varchar(64),
       m6_unit varchar(64),
       m6_in bigint,
       m6_out bigint,
       m7_name varchar(64),
       m7_unit varchar(64),
       m7_in bigint,
       m7_out bigint,
       m8_name varchar(64),
       m8_unit varchar(64),
       m8_in bigint,
       m8_out bigint,
       m9_name varchar(64),
       m9_unit varchar(64),
       m9_in bigint,
       m9_out bigint,
       m10_name varchar(64),
       m10_unit varchar(64),
       m10_in bigint,
       m10_out bigint,
       m11_name varchar(64),
       m11_unit varchar(64),
       m11_in bigint,
       m11_out bigint,
       m12_name varchar(64),
       m12_unit varchar(64),
       m12_in bigint,
       m12_out bigint,
       m13_name varchar(64),
       m13_unit varchar(64),
       m13_in bigint,
       m13_out bigint,
       m14_name varchar(64),
       m14_unit varchar(64),
       m14_in bigint,
       m14_out bigint,
       m15_name varchar(64),
       m15_unit varchar(64),
       m15_in bigint,
       m15_out bigint
) execute 'cat gpmon.1/iterators_now.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table iterators_tail (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
       segid int,
       pid int,
       nid int,
       pnid int,
       hostname varchar(64),
       ntype varchar(64),
       nstatus varchar(64),
       tstart timestamp(0),
       tduration int,
       pmemsize bigint,
       pmemmax bigint,
       memsize bigint,
       memresid bigint,
       memshare bigint,
       pfaults bigint,
       pfaults_elapsed bigint,
       cpu_elapsed bigint,
       cpu_currpct float,
       phase varchar(64),
       m0_name varchar(64),
       m0_unit varchar(64),
       m0_in bigint,
       m0_out bigint,
       m1_name varchar(64),
       m1_unit varchar(64),
       m1_in bigint,
       m1_out bigint,
       m2_name varchar(64),
       m2_unit varchar(64),
       m2_in bigint,
       m2_out bigint,
       m3_name varchar(64),
       m3_unit varchar(64),
       m3_in bigint,
       m3_out bigint,
       m4_name varchar(64),
       m4_unit varchar(64),
       m4_in bigint,
       m4_out bigint,
       m5_name varchar(64),
       m5_unit varchar(64),
       m5_in bigint,
       m5_out bigint,
       m6_name varchar(64),
       m6_unit varchar(64),
       m6_in bigint,
       m6_out bigint,
       m7_name varchar(64),
       m7_unit varchar(64),
       m7_in bigint,
       m7_out bigint,
       m8_name varchar(64),
       m8_unit varchar(64),
       m8_in bigint,
       m8_out bigint,
       m9_name varchar(64),
       m9_unit varchar(64),
       m9_in bigint,
       m9_out bigint,
       m10_name varchar(64),
       m10_unit varchar(64),
       m10_in bigint,
       m10_out bigint,
       m11_name varchar(64),
       m11_unit varchar(64),
       m11_in bigint,
       m11_out bigint,
       m12_name varchar(64),
       m12_unit varchar(64),
       m12_in bigint,
       m12_out bigint,
       m13_name varchar(64),
       m13_unit varchar(64),
       m13_in bigint,
       m13_out bigint,
       m14_name varchar(64),
       m14_unit varchar(64),
       m14_in bigint,
       m14_out bigint,
       m15_name varchar(64),
       m15_unit varchar(64),
       m15_in bigint,
       m15_out bigint
) execute 'cat gpmon.1/iterators_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table _iterators_tail (
       ctime timestamp(0),
       tmid int,
       xid int,
       cid int,
       -- ssid int,    -- for 3.2, add gp_session_id
       -- ccnt int,    -- for 3.2, add gp_command_count
       segid int,
       pid int,
       nid int,
       pnid int,
       hostname varchar(64),
       ntype varchar(64),
       nstatus varchar(64),
       tstart timestamp(0),
       tduration int,
       pmemsize bigint,
       pmemmax bigint,
       memsize bigint,
       memresid bigint,
       memshare bigint,
       pfaults bigint,
       pfaults_elapsed bigint,
       cpu_elapsed bigint,
       cpu_currpct float,
       phase varchar(64),
       m0_name varchar(64),
       m0_unit varchar(64),
       m0_in bigint,
       m0_out bigint,
       m1_name varchar(64),
       m1_unit varchar(64),
       m1_in bigint,
       m1_out bigint,
       m2_name varchar(64),
       m2_unit varchar(64),
       m2_in bigint,
       m2_out bigint,
       m3_name varchar(64),
       m3_unit varchar(64),
       m3_in bigint,
       m3_out bigint,
       m4_name varchar(64),
       m4_unit varchar(64),
       m4_in bigint,
       m4_out bigint,
       m5_name varchar(64),
       m5_unit varchar(64),
       m5_in bigint,
       m5_out bigint,
       m6_name varchar(64),
       m6_unit varchar(64),
       m6_in bigint,
       m6_out bigint,
       m7_name varchar(64),
       m7_unit varchar(64),
       m7_in bigint,
       m7_out bigint,
       m8_name varchar(64),
       m8_unit varchar(64),
       m8_in bigint,
       m8_out bigint,
       m9_name varchar(64),
       m9_unit varchar(64),
       m9_in bigint,
       m9_out bigint,
       m10_name varchar(64),
       m10_unit varchar(64),
       m10_in bigint,
       m10_out bigint,
       m11_name varchar(64),
       m11_unit varchar(64),
       m11_in bigint,
       m11_out bigint,
       m12_name varchar(64),
       m12_unit varchar(64),
       m12_in bigint,
       m12_out bigint,
       m13_name varchar(64),
       m13_unit varchar(64),
       m13_in bigint,
       m13_out bigint,
       m14_name varchar(64),
       m14_unit varchar(64),
       m14_in bigint,
       m14_out bigint,
       m15_name varchar(64),
       m15_unit varchar(64),
       m15_in bigint,
       m15_out bigint
) execute 'cat gpmon.1/_iterators_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');



create external web table master_data_dir (hostname text, dir text)
execute E'python -c "import socket, os; print socket.gethostname() + \\\\"|\\\\" + os.getcwd()"' on master
format 'csv' (delimiter '|');

'''


######################################################################
#
#  create the partitions
#
######################################################################

START = date(2008, 6, 1)
STOP  = date(2010, 1, 1)
ONEDAY = timedelta(1)



def gen(tabname):
    thismonth = START
    while thismonth < STOP:
        if thismonth.month == 12:
            nextmonth = date(thismonth.year + 1, 1, 1)
        else:
            nextmonth = date(thismonth.year, thismonth.month + 1, 1)
        
        name = '%04d%02d' % (thismonth.year, thismonth.month)
        range = ('%04d-%02d-%02d:00:00:00' % (thismonth.year, thismonth.month, 1),
                 '%04d-%02d-%02d:00:00:00' % (nextmonth.year, nextmonth.month, 1))
        stmt = ('''CREATE TABLE %s_%s
	    (CHECK (ctime >= '%s'::timestamp without time zone and
                    ctime < '%s'::timestamp without time zone))
            INHERITS (%s);''' % (tabname, name, range[0], range[1], tabname))
        print stmt
        thismonth = nextmonth

gen('metrics_history');
gen('queries_history');
gen('iterators_history');

print 'revoke all on database gpmon_1 from public;'
