#!/usr/bin/perl
use strict;
use warnings;
use Safe;

# known bad versions of Safe, installation /should/ have made sure
# this never occured
my $v = $Safe::VERSION;
die "Safe version $v not supported" if ($v < 2.0899);

# This script is called on the segment hosts as follows
#   1) mapred.pl is called on the master host
#   2) mapred.pl generates an EXTERNAL WEB TABLE with
#      > EXEC 'docmap.pl ''$user_code'' ''$key'' file1 file2 file3 ...'
#   3) mapred.pl distributes the files to the segment hosts
#   4) A query is issued against the EXTERNAL WEB TABLE
#   5) this script gets called

# First parameter is the user code to execute.
my $user_code = shift;
$user_code = "$user_code; return undef";

# User code will be executed in a Safe enclosure, we use the same
# settings as the plperl language.
my $isolation = new Safe 'User';
$isolation->permit_only(':default');
$isolation->permit(qw[:base_math !:base_io sort time]);

# Second parameter is the 'key' passed to mapred.pl
#   - we may need extra code to allow additional parameters?
our $key = shift;

# Rest of the parameters are filenames
our $filename;
my @files = @ARGV;

# share key, and filename
$isolation->share('$key');

foreach $filename (@files) {
	$isolation->share('$filename');    

    # each segment only recieved the files it is intended to handle
    # assume that if a file doesn't exist that someone else is handling it
    next unless(-f $filename);
    
    # something is weird if it exists and we don't have permission.
    # We could use some better error handling, for now silently do nothing
    next unless(-r $filename);

    # For now we loop the file outside the scope of the user code and
    # just pass the input in line by line.
    open FILE, "<$filename";

    while (<FILE>) {
		chomp;
        my $output = $isolation->reval($user_code);
        die $@ if ($@);
        
        # web table takes input printed to STDOUT
        print $output if (defined($output));
    }
    close FILE;
}

