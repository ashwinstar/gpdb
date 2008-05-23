#!/usr/bin/perl
#
# mapred.pl - Greenplum MapReduce driver script
#
# copyright (c) 2007
# Author: Jeffrey I Cohen, Caleb Welton
#

=head1 NAME

B<mapred.pl> - Greenplum MapReduce driver script

=head1 SYNOPSIS

B<mapred.pl> [options] file [parameter-list]

=head1 DESCRIPTION

B<mapred.pl> reads an input file and executes a parallel MapReduce
job in the Greenplum database.

=head1 OPTIONS

=over 8

=item B<-help>  : Print a brief help message and exits.

=item B<-man>   : Print the manual page and exits.

=item B<-print> : Print the sql dump and exits.

=item B<-explain> : Print the explain plan and exits.

=back

=head1 AUTHORS

Jeffrey I Cohen, Caleb Welton

Copyright (c) 2008 GreenPlum.  All rights reserved.  

Address bug reports and comments to: cwelton@greenplum.com

=cut

# add the directory that mapred.pl is in to the include path
BEGIN {
	my $path = $0;
	$path =~ s/[^\/]*$/..\/lib/;
	unshift @INC, $path;
}

use strict;
use warnings;
use Pod::Usage;
use Getopt::Long;
use GPMapReduce::Parser;

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
my $file = shift;
pod2usage(-exitstatus => 1) unless ($file);

(-d $file) and die "ERROR: '$file' is a directory\n";
(-f $file) or  die "ERROR: No such file '$file'\n";
(-r $file) or  die "ERROR: No read permissions for '$file'\n";

# Get the rest of the parameters
my $nparam = $#ARGV + 1;
my @param  = @ARGV;
if ($nparam eq 0) 
{
    $nparam = 1;
    @param = (undef);
}

#
# Read in the and parse the file
#   currently outputs some pretty useless errors if the parse fails
#
my $yaml;
my $docs;
{
    if (eval " require YAML::Tiny " )
    {
        $yaml = YAML::Tiny->new;
        $docs = $yaml->read($file);
    }
    elsif (eval " require YAML " )
    {
        $yaml = YAML::Base->new();
        $docs = YAML::LoadFile($file);
    }
    else
    {
        die("Fatal Error: The required package YAML is not installed -- please download it from www.cpan.org\n");
    }
}

# A parse of a single file could have resulted in multiple
# yaml "documents" treat each document as a distinct entity.	
my $parser = GPMapReduce::Parser->new;
foreach my $ydoc (@$docs)
{
    my $doc = $parser->Parse($ydoc, @param);
    my $sql = $doc->GenerateSQL($explain);

    if ($print == 0)
    {
	open(TMP, " | psql postgres -qtX") || die "can't open pipe";
	print TMP $sql;
	close TMP;
    }
    else
    {
	print $sql;
    }
}
exit(0);
