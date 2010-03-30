--  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
--  Gpperfmon 4.x Schema

\c gpperfmon;

create table segment_history (
       ctime timestamp(0) not null, -- record creation time
       dbid int not null,           -- segment database id
       hostname varchar(64) not null, -- hostname of system this metric belongs to
       dynamic_memory_used bigint not null, -- bytes of dynamic memory used by the segment
       dynamic_memory_available bigint not null -- bytes of dynamic memory available for use by the segment
) 
with (fillfactor=100)
distributed by (ctime)
partition by range (ctime)(start (date '2009-09-01') end (date '2011-09-01') EVERY (interval '1 month'));

create external web table segment_now (
       like segment_history
) execute 'cat gpperfmon/data/segment_now.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table segment_tail (
       like segment_history
) execute 'cat gpperfmon/data/segment_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


create external web table _segment_tail (
        like segment_history
) execute 'cat gpperfmon/data/_segment_tail.dat 2> /dev/null || true' on master format 'text' (delimiter '|' NULL as 'null');


DROP VIEW IF EXISTS dynamic_memory_info;
CREATE VIEW dynamic_memory_info as select
		segment_history.ctime, segment_history.hostname, 
		round(sum(segment_history.dynamic_memory_used)/1024/1024, 2) AS dynamic_memory_used_mb, 
		round(sum(segment_history.dynamic_memory_available)/1024/1024, 2) AS dynamic_memory_available_mb
		FROM segment_history
		GROUP BY segment_history.ctime, segment_history.hostname;

DROP VIEW IF EXISTS memory_info;
CREATE VIEW memory_info as select
		system_history.ctime, 
		system_history.hostname, 
		round(system_history.mem_total/1024/1024, 2) as mem_total_mb,
 		round(system_history.mem_used/1024/1024, 2) as mem_used_mb, 
		round(system_history.mem_actual_used/1024/1024, 2) as mem_actual_used_mb, 
		round(system_history.mem_actual_free/1024/1024, 2) as mem_actual_free_mb, 
		round(system_history.swap_total/1024/1024, 2) as swap_total_mb, 
		round(system_history.swap_used/1024/1024, 2) as swap_used_mb, 
		dynamic_memory_info.dynamic_memory_used_mb as dynamic_memory_used_mb, 
		dynamic_memory_info.dynamic_memory_available_mb as dynamic_memory_available_mb
		FROM system_history, dynamic_memory_info 
		WHERE system_history.hostname = dynamic_memory_info.hostname 
		AND system_history.ctime = dynamic_memory_info.ctime;


