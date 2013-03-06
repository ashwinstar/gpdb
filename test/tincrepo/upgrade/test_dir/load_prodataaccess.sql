create function func1(int, int) returns int as
$$
  select $1 + $2;
$$ language sql; 

create function func2(int, int) returns varchar as $$
declare
        v_name varchar(20) DEFAULT 'zzzzz';
begin
        select relname from pg_class into v_name where oid=$1;
        return v_name;
end;
$$ language plpgsql;

