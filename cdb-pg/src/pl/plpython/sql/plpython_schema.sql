
CREATE TABLE users (
	fname text not null,
	lname text not null,
	username text,
	userid serial,
	PRIMARY KEY(lname, fname) 
	) ;

CREATE INDEX users_username_idx ON users(username);
CREATE INDEX users_fname_idx ON users(fname);
CREATE INDEX users_lname_idx ON users(lname);
CREATE INDEX users_userid_idx ON users(userid);


CREATE TABLE taxonomy (
	id serial primary key,
	name text
	) ;

CREATE TABLE entry (
	accession text not null primary key,
	eid serial,
	txid int2 not null
	) ;

CREATE TABLE sequences (
	eid int4 not null,
	pid serial primary key,
	product text not null,
	sequence text not null,
	multipart bool default 'false'
	) ;
CREATE INDEX sequences_product_idx ON sequences(product) ;

CREATE TABLE xsequences (
	pid int4 not null,
	sequence text not null
	) DISTRIBUTED BY (pid);
CREATE INDEX xsequences_pid_idx ON xsequences(pid) ;

CREATE TABLE unicode_test (
	testvalue  text NOT NULL
) DISTRIBUTED BY (testvalue);

CREATE TABLE table_record (
	first text,
	second int4
	) DISTRIBUTED BY (first);

CREATE TYPE type_record AS (
	first text,
	second int4
	) ;
