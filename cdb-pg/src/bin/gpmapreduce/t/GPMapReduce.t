# Before `make install' is performed this script should be runnable with
# `make test'. After `make install' it should work as `perl GPMapReduce-MapReduce.t'

#########################

use Test;
BEGIN { 
  plan tests => 12,
  todo => []              # tests expecting failure
 };


## Test 1 - Module load
my $error;
use GPMapReduce::Document;
ok(1);

## Test 2-3 - Obect Creation
my $mr = new GPMapReduce::Document();
ok(defined $mr, 1); 
my $map = new GPMapReduce::Mapper(Name => 'MyMap');
ok(defined $map, 1);

# Test 4 - Simple AddMapper Call
ok($mr->AddMapper(Mapper => $map), undef);

# Test 5-8 - Bad parameter passing
ok(eval{$mr->AddMapper()}, undef);
ok(eval{$mr->AddMapper($map, 4)}, undef);
ok(eval{$mr->AddMapper(Mapper => $map, Junk => 0)}, undef);
ok(eval{$mr->AddMapper(Junk => $map)}, undef);

# Test 9-12 - Typechecking
sub tcheck($$$) {
  my ($x,$p,$s) = @_;
  eval{GPMapReduce::Document::AddMapper($x, Mapper => $p)}; 
  return "pass" if ($@ =~ /$s/);
  return $@;
}

ok(tcheck($mr, 4, "Mapper => SCALAR :: expected GPMapReduce::Mapper"), 
   "pass");
ok(tcheck($mr, $mr, 
   "Mapper => GPMapReduce :: expected GPMapReduce::Mapper"), 
   "pass");
ok(tcheck(undef, undef, "self => undef :: expected GPMapReduce"), 
   "pass");
ok(tcheck($map, $map, 
   "self => GPMapReduce::Mapper :: expected GPMapReduce"),
   "pass");

