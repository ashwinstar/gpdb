-- --------------------------------------------------------------------
--
-- cdb_schema.sql
--
-- Define CDB administrative schema and several SQL functions to aid 
-- in maintaining the CDB administrative schema.
--
-- $Id$
--
-- --------------------------------------------------------------------

set log_min_messages = 'WARNING';

-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- Create the schema and ensure PUBLIC access
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
CREATE SCHEMA cdb;
COMMENT ON SCHEMA cdb
    IS 'repository for CDB service functions and related schema objects';
GRANT USAGE ON SCHEMA cdb TO PUBLIC;


-- A row of table HOST represents a host machine in the CDB.
--
-- Attribute hostname is a component of the URL for PostgreSQL databases
-- (segment databases) served by listeners that run on the host machine.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.HOST
(
	hostid integer,
	hostname text not null,
	amsport integer not null,
	
	primary key (hostid),
	unique (hostname)
);


-- A row of table LISTENER represents a listener (postmaster process) used
-- by the running CDB. 
--
-- Attribute port is a component of the URL for PostgreSQL databases 
-- (segment databases) served by the listener.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.LISTENER
(
	listenerid integer,
	hostid integer not null,
	port integer not null,
	datadir text not null,
	
	primary key (listenerid),
	foreign key (hostid) references CDB.host
);


-- A row of table SEGMENT represents a segment of the logical database
-- stored in the CDB.
--
-- Attribute segindex has a value from interval 0..(N-1) where N is the
-- number of segments (not segment databases) in the CDB.
--
-- Never the less, the database name used for the segment databases the
-- implement the segment is fixed here, thus implying that the a given
-- listener can serve at most one segment database per segment.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.SEGMENT
(
	segindex integer,
	dbname text not null,
	dbpswd text not null,
	
	primary key (segindex),
	unique (dbname)
);



-- A row of table SEGDB represents a segment database in the CDB.
--
-- Attribute segindex has a value from interval 0..(N-1) where N is the
-- number of segments (not segment databases) in the CDB.
--
-- The database name used for the segment database is determined by the
-- segment that the database implements.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.SEGDB
(
	segindex integer,
	isprimary boolean,
	listenerid integer not null,
	isvalid boolean not null,
	
	primary key (segindex, isprimary),
	unique (segindex, listenerid),
	foreign key (segindex) references CDB.segment(segindex),
	foreign key (listenerid) references CDB.listener(listenerid)
);


-- The sole row of table CDB represents (1) the CDB as a whole and (2) the
-- segment database in which it appears.
--
-- All segment databases in a given CDB have the same values for attributes 
-- cdbName and numSegments.  The remaining attributes  are specific to the 
-- segment database.
--
-- No two segment databases in a given CDB can match in all attributes of
-- this table.

create table CDB.CDB
(
	cdbname name,
	numSegments integer not null,
	listenerId integer not null,
	segindex integer not null,
	isprimary boolean not null,
	
	primary key (cdbname),
	foreign key( segIndex, isprimary) references CDB.segdb(segIndex,isprimary),
	foreign key (segindex) references CDB.segment(segindex)
);

create table CDB.VERSION
(
    schemaversion integer
);

INSERT INTO CDB.VERSION VALUES ( 1 );


-- A row of table SPACECATEGORY represents a logical category of physical 
-- storage within the CDB.
--
-- There are three space categories required in every CDB, thus each 
-- actual SPACEATEGORY table is guaranteed to have three rows with 
-- known values: TEMP, FAST, and SLOW.  CDBs implemented in special
-- hardware environments (e.g. solid state disks or whatever) may have 
-- additional rows.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.SPACECATEGORY
(
	category text,
	remark text not null,
	
	primary key (category)
);

insert into CDB.SPACECATEGORY values ('TEMP','Sort overflow area');
insert into CDB.SPACECATEGORY values ('FAST','Primary data area');
insert into CDB.SPACECATEGORY values ('SLOW','Secondary data area');


-- A row of table TABLESPACE represents a CDB tablespace (not a PostgreSQL 
-- tablespace).
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.TABLESPACE
(
	spaceid integer,
	spacename text not null,
	category text not null,
	
	primary key (spaceid),
	unique (spacename),
	foreign key (category) references CDB.spacecategory(category)
);


-- A row of table EXTENT represents a a distinct piece of physical storage
-- (e.g., a disk or disk partition on a specific host machine).
--
-- The idea is to represent how the administrator has carved up the available
-- disk.  This "carving up" is managed and enforced externally.  The table
-- EXTENT merely informs CDB of the storage available to it.
--
-- Each segment database in a given CDB contains an identical copy 
-- of this table.

create table CDB.EXTENT
(
	listenerid integer,
	path text not null,
	category text not null,
	spaceid integer,
	pg_spacename text not null, -- system assigned
	
	primary key (listenerid,path),
	foreign key (listenerid) references CDB.listener(listenerid),
	foreign key (category) references CDB.spacecategory(category),
	foreign key (spaceid) references CDB.tablespace(spaceid)
);


-- A row of POLICY represents a database object known to the CDB.
--
-- There are two uses for a row of POLICY suggesting that, perhaps, 
-- it should be separated into two tables: 
--
--  1. It supports remapping of OIDs.
--  2. It describes policy under which table data is distributed 
--     (which is closely related to the type of a table).
--
-- (Note that POLICY it might better be noted by altering pg_class.)
--
-- Attribute policytype has three possible values:
--
-- 'p' - the object is a table and is partitioned
-- 'r' - the object is a table and is replicated
-- 's' - the object is special, e.g., managed by PostgreSQL or CDB.
--
-- (It seems likely that the catch-all type 's' should be replaced by a
-- small set of more specific type, but it is unclear at present what they
-- should be.)
--
-- Each segment database in a given CDB contains a unique version of table
-- POLICY, since it relates local to global identifiers.  On the other hand,
-- every segment database will have an identical projection of POLICY 
-- defined by  
--
--     SELECT DISTINCT GLOBALID, POLICYTYPE, ATTRNUMS
--     FROM CDB.POLICY


create table CDB.POLICY
(
	globalid integer,
	localoid oid,
	policytype char(1) not null,
	attrnums integer[],
	
	primary key (globalid),
	unique (localoid), -- logically a foreign key to pg_class
	constraint policytypes check (policytype in ('p','r','s'))
);

set log_min_messages to DEFAULT;


