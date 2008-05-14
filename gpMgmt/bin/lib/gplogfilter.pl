#!/usr/bin/perl
#
# $Header$
#
# copyright (c) 2008
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use Getopt::Long;
use Data::Dumper;
use strict;
use warnings;

=head1 NAME

B<gplogfilter.pl> - filter a log file by date

=head1 SYNOPSIS

B<gplogfilter> [options] 

Options:

    -help            brief help message
    -man             full documentation
    -start_date      start date for filtering

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-start_date>
    
    All log entries which take place prior to the start date are
    filtered out.  You can specify an explicit date like "2008-05-24"
    or a delta like "5 days" (implying the previous 5 days).  If not
    specified, the start date is 3 days prior to the current time, ie
    the previous 3 days. 

=back

=head1 DESCRIPTION

gplogfilter reads a file (specified as a command line argument or from
stdin) and filters out all lines older than the specified start date.
gplogfilter assumes that the input file is fully-ordered, so once it
finds a line that matches the start date, it stops filtering and
returns the remainder of the file.  Each line must have a prefix of
the date in YYYY-MM-DD format at the beginning of the line, eg:

2008-05-09 12:00:04.473455 PDT|||1183|postmaster|:-LOG: reserving 5 semaphores

Prior to matching the start date, gplogfilter simply skips lines that
do not match this format, so if your input lines do not have a date
prefix, gplogfilter will skip every line and not produce any output.

=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2008 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

my $glob_id = "";

my $glob_tab;
my $glob_stime;


BEGIN {
    my $man  = 0;
    my $help = 0;
    my $table;
    my $std;

    GetOptions(
               'help|?' => \$help, man => \$man, 
               "table=s" => \$table,
               "start_date=s" => \$std
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    if (defined($std))
    {
        if ($std =~ m/^\d\d\d\d\-\d(\d?)\-\d(\d?)/)
        {
            my ($yy,$mm,$dd) = ($std =~ m/^(\d\d\d\d)\-(\d+)\-(\d+)/);
            $glob_stime = sprintf ("%d%02d%02d", $yy, $mm, $dd);
        }
        elsif ($std =~ m/^\d+/)
        {
            my @foo = ($std =~ m/^(\d+)/);
            my $currtime = time();
            my ($dd,$mm,$yy) = (localtime(time()-($foo[0])*86400 ))[3..5]; 
            $glob_stime = sprintf ("%d%02d%02d", $yy+1900, $mm+1, $dd);
        }
        else
        {
            die ("Error: invalid start time $std\n");
        }
    }    
    else
    {
        # default is 3 days
        my $currtime = time();
        my ($dd,$mm,$yy) = (localtime(time()-3*86400 ))[3..5]; 
        $glob_stime = sprintf ("%d%02d%02d", $yy+1900, $mm+1, $dd);
    }

#    print "loading...\n" ;
}


if (1)
{
    print "Filter start date: ", $glob_stime, "\n";

    my $printall = 0;

    while (<>)
    {
        my $lin = $_;

        if ($printall)
        {
            print $lin;
            next;
        }

        if ($lin =~ m/^\d\d\d\d\-\d\d\-\d\d/)
        {
            my ($yy,$mm,$dd) = ($lin =~ m/^(\d\d\d\d)\-(\d\d)\-(\d\d)/);
            my $logtime = sprintf ("%d%02d%02d", $yy, $mm, $dd);

#            print $logtime, "\n";

            next if ($logtime < $glob_stime);

            $printall = 1;
            print $lin;
        }

        
    }
}

exit();
