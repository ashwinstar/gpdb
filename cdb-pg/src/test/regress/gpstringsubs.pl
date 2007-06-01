#!/usr/bin/perl
#
# $Header$
#
# Copyright (c) 2007 GreenPlum.  All rights reserved.  
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use Getopt::Long;
use strict;
use warnings;
use POSIX;
use File::Spec;
use Env;
use Data::Dumper;

=head1 NAME

B<gpstringsubs.pl> - GreenPlum string substitution 

=head1 SYNOPSIS

B<gpstringsubs.pl> filename

Options:

    -help            brief help message
    -man             full documentation
    -connect         psql connect parameters

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-connect>

    psql connect string, e.g:

    -connect '-p 11000 template1'

        
=back

=head1 DESCRIPTION

gpstringsubs find tokens in an input file and replaces them (in place)
with specified or environmental values, supplementing the pg_regress
"convert_sourcefiles_in" function.

=over 8

=item hostname

gpstringsubs finds the hostname for segment 0 from the
gp_configuration table and replaces all instances of the token
B<@hostname@>.

=back


=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2007 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

    my $glob_id = "";
    my $glob_connect;


BEGIN {
    my $man  = 0;
    my $help = 0;
    my $conn;

    GetOptions(
               'help|?' => \$help, man => \$man,
               "connect=s" => \$conn
               )
        or pod2usage(2);


    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    $glob_connect = $conn;

    my $porti = $PGPORT;

#    $porti = 11000
#        unless (defined($porti));

    $glob_connect = "-p $porti template1"
        unless (defined($glob_connect));

#    print "loading...\n" ;                                                     
}


# convert a postgresql psql formatted table into an array of hashes
sub tablelizer
{
    my ($ini, $got_line1) = @_;

    # first, split into separate lines, the find all the column headings

    my @lines = split(/\n/, $ini);

    return undef
        unless (scalar(@lines));

    # if the first line is supplied, then it has the column headers,
    # so don't try to find them (or the ---+---- separator) in
    # "lines"
    my $line1 = $got_line1;
    $line1 = shift @lines
        unless (defined($got_line1));

    # look for <space>|<space>
    my @colheads = split(/\s+\|\s+/, $line1);

    # fixup first, last column head (remove leading,trailing spaces)

    $colheads[0] =~ s/^\s+//;
    $colheads[0] =~ s/\s+$//;
    $colheads[-1] =~ s/^\s+//;
    $colheads[-1] =~ s/\s+$//;

    return undef
        unless (scalar(@lines));
    
    shift @lines # skip dashed separator (unless it was skipped already)
        unless (defined($got_line1));
    
    my @rows;

    for my $lin (@lines)
    {
        my @cols = split(/\|/, $lin, scalar(@colheads));
        last 
            unless (scalar(@cols) == scalar(@colheads));

        my $rowh = {};

        for my $colhdcnt (0..(scalar(@colheads)-1))
        {
            my $rawcol = shift @cols;

            $rawcol =~ s/^\s+//;
            $rawcol =~ s/\s+$//;

            my $colhd = $colheads[$colhdcnt];
            $rowh->{($colhdcnt+1)} = $rawcol;
        }
        push @rows, $rowh;
    }

    return \@rows;
}


if (1)
{
    my $psql_str = "psql ";
    
    $psql_str .= $glob_connect
        if (defined($glob_connect));

#    $psql_str .= " -c \'\\d $glob_tab \'";

    $psql_str .= " -c \'select * from gp_configuration \'";

#    print $psql_str, "\n";

    my $tabdef = `$psql_str`;

#    print $tabdef;

    my $mpp_config_table = tablelizer($tabdef);

#    print Data::Dumper->Dump([$mpp_config_table]);

    my $hostname= "localhost";

    for my $rowh (@{$mpp_config_table})
    {
        if (($rowh->{1} == 0) && # content (seg 0)
            ($rowh->{2} =~ m/t/) && # defined primary
            ($rowh->{4} =~ m/t/) && # isprimary
            ($rowh->{5} =~ m/t/)) # valid
        {
            $hostname = $rowh->{6};
            last;
        }
        
    }

    my $filnam = $ARGV[0];

    my $hostexp = '\\@hostname\\@';

#    print "$filnam\n";
    system "perl -i -ple \' s/$hostexp/$hostname/gm\' $filnam\n";

    exit(0);
}
