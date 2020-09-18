create or replace language plpython3u;

--
-- pg_basebackup:
--   host: host of the gpdb segment to back up
--   port: port of the gpdb segment to back up
--   slotname: desired slot name to create and associate with backup
--   datadir: destination data directory of the backup
--   forceoverwrite: overwrite the destination directory if it exists already
--   xlog_method: (stream/fetch) how to obtain XLOG segment files from source
--
-- usage: `select pg_basebackup('somehost', 12345, 'some_slot_name', '/some/destination/data/directory')`
--
create or replace function pg_basebackup(host text, dbid int, port int, slotname text, datadir text, force_overwrite boolean, xlog_method text) returns text as $$
    import subprocess
    import os
    cmd = 'pg_basebackup --checkpoint=fast -h %s -p %d -R -D %s --target-gp-dbid %d' % (host, port, datadir, dbid)

    if slotname is not None:
        cmd += ' --slot %s' % (slotname)

    if force_overwrite:
        cmd += ' --force-overwrite'

    if xlog_method == 'stream':
        cmd += ' --xlog-method stream'
    elif xlog_method == 'fetch':
        cmd += ' --xlog-method fetch'
    else:
        plpy.error('invalid xlog method')

    try:
        # Unset PGAPPNAME so that the pg_stat_replication.application_name is not affected
        if os.getenv('PGAPPNAME') is not None:
            os.environ.pop('PGAPPNAME')
        results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
    except subprocess.CalledProcessError as e:
        results = str(e) + "\ncommand output: " + e.output

    return results
$$ language plpython3u;


create or replace function count_of_items_in_directory(user_path text) returns text as $$
       import subprocess
       cmd = 'ls {user_path}'.format(user_path=user_path)
       results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
       return len([result for result in results.splitlines() if result != ''])
$$ language plpython3u;

create or replace function count_of_items_in_database_directory(user_path text, database_oid oid) returns int as $$
       import subprocess
       import os
       directory = os.path.join(user_path, str(database_oid))
       cmd = 'ls ' + directory
       results = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True).replace(b'.', b'').decode()
       return len([result for result in results.splitlines() if result != ''])
$$ language plpython3u;

create or replace function validate_tablespace_symlink(datadir text, tablespacedir text, dbid int, tablespace_oid oid) returns boolean as $$
    import os
    return os.readlink('%s/pg_tblspc/%d' % (datadir, tablespace_oid)) == ('%s/%d' % (tablespacedir, dbid))
$$ language plpython3u;
