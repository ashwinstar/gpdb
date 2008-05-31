# Before `make install' is performed this script should be runnable with
# `make test'. After `make install' it should work as `perl GPMapReduce-MapReduce.t'

#########################

use Test;
BEGIN { 
  plan tests => 21,
  todo => []              # tests expecting failure
 };

## Test 1 - Load the Module
use GPMapReduce::Mapper;
ok(1);

## Test 2 - Create a Mapper
my $map = new GPMapReduce::Mapper(Name => 'Test', Body => 'return undef;');
$map->CheckInput([]);
ok(defined $map, 1); 


## Test 3-6 - Setting perl body
my $f = q[ my ($key, $value) = @_; return (key=>$key,value=>$value) ];
$map->SetFunction();
ok($map->{Body}, undef);
$map->SetFunction(Body => $f);
ok($map->{Body} =~ /my..key, .value. = ._;/gms, 1);
$map->SetFunction(Body => $f, Language => 'perl');
ok($map->{Body} =~ /my..key, .value. = ._;/gms, 1);
eval{$map->SetFunction(Function => $f, Language => 'perl')};
ok($@ =~ /Error: Parameter Type Mismatch/gms,  1);

## Test 7-12 - Setting other language Body
$map->SetFunction(Body => 'unparsed string', Language => 'C');
ok($map->{Body} =~ /unparsed string/gms, 1);
$map->SetFunction(Body => 'unparsed string', Language => 'R');
ok($map->{Body} =~ /unparsed string/gms, 1);
$map->SetFunction(Body => 'unparsed string', Language => 'pgsql');
ok($map->{Body} =~ /unparsed string/gms, 1);
$map->SetFunction(Body => 'unparsed string', Language => 'sql');
ok($map->{Body} =~ /unparsed string/gms, 1);
$map->SetFunction(Body => 'unparsed string', Language => 'python');
ok($map->{Body} =~ /unparsed string/gms, 1);
eval{$map->SetFunction(Body => 'unparsed string', Language => 'badlang')};
ok($@ =~ /Error: Bad Parameter \( Language =>/gms, 1);

## Test 13-17 - Setting Body by function pointer
sub f { my ($key,$value) = @_; return {key=>$key, value=>$value};};
$map->SetFunction(Function => \&f);
ok($map->{Body} =~ /return {'key', \$key, 'value', \$value}/gms,   1);
eval{$map->SetFunction(Body => \&f)};
ok($@ =~ /Error: Parameter Type Mismatch/gms,  1);
eval{$map->SetFunction(Function => \&f, Language => 'perl')};
ok($@ =~ /Error: Incompatable Parameters - Function \+ Body/gms,  1);
eval{$map->SetFunction(Function => \&f, Body => 'some value')};
ok($@ =~ /Error: Incompatable Parameters - Function \+ Body/gms,  1);
eval{$map->SetFunction(Function => \&f, Language => 'perl')};
ok($@ =~ /Error: Incompatable Parameters/gms, 1);

## Test 18-19 - Generating output
my $drop = $map->SQLDrop();
ok($drop eq <<END, 1);
DROP FUNCTION IF EXISTS Test(text, text) CASCADE;
DROP TYPE IF EXISTS Test_Return CASCADE;
END

my $create = $map->SQLCreate();
ok($create eq <<END, 1);
CREATE TYPE Test_Return as (key text, value text);
CREATE FUNCTION Test(key text, value text) RETURNS Test_Return AS \$\$
{
    my(\$key, \$value) = \@_;
    return {'key', \$key, 'value', \$value};
}
\$\$ LANGUAGE plperl;
END

## Test 20-21 - Clearing the function Body
$map->SetFunction();
ok($map->{Body}, undef);
ok($map->{Lang}, undef);
