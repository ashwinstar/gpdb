#!/usr/bin/perl
#
# fixup.pl - Greenplum MapReduce fixup script
#
# copyright (c) 2007
# Author: Jeffrey I Cohen, Caleb Welton
#

use strict;
use warnings;
use Pod::Usage;
use Getopt::Long;
use Cwd;

my $glob_id = "";
my $man  = 0;
my $help = 0;
my $print = 0;
my $explain = 0;

GetOptions('help|?' => \$help, 
		   man => \$man,
		   print => \$print,
	       explain => \$explain,)
	or pod2usage(2);
pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

# Check that input file exists and is readable



if (1)
{
	my $curdir   = getcwd();
    my $whoam    = `whoami`;
	my $hostname = `hostname`;
	chomp $whoam;
	chomp $hostname;

	# We assume that this script is run by the make file in example
	my $datadir = "$curdir/data";

    for my $file (@ARGV)
    {
        pod2usage(-exitstatus => 1) unless ($file);

        (-d $file) and die "ERROR: '$file' is a directory\n";
        (-f $file) or  die "ERROR: No such file '$file'\n";
        (-r $file) or  die "ERROR: No read permissions for '$file'\n";

        my $outfile = $file;
        $outfile =~ s/\.in$//g;

		open(INPUT, "<$file");
		open(OUTPUT, ">$outfile");
		while (<INPUT>) {
			s/\<USER\>/$whoam/gm; 
			s/\<DATA_DIRECTORY\>/$hostname$datadir/gm;
			print OUTPUT
		}
		close(INPUT);
		close(OUTPUT);
    }
}

exit(0);
