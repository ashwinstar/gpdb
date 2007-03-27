#!/usr/bin/perl
#
# $Header$
#
# copyright (c) 2007
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use Getopt::Long;
use Data::Dumper;
use strict;
use warnings;

=head1 NAME

B<atmsort.pl> - [A] [T]est [M]echanism Sort: sort the contents of SQL log files to aid diff comparison

=head1 SYNOPSIS

B<atmsort.pl> [options] logfile [logfile...]

Options:

    -help            brief help message
    -man             full documentation

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=back

=head1 DESCRIPTION

atmsort reads sql log files from STDIN and sorts the query output for
all SELECT statements that do *not* have an ORDER BY, writing the
result to STDOUT.  This change to the log facilitates diff comparison,
since unORDERed query output does not have a guaranteed order.  Note
that for diff to work correctly, statements that do use ORDER BY must
have a fully-specified order.

The log content must look something like:

 SELECT a, b, c, d 
   from foo 
   ORDER BY 1,2,3,4;
      a      |        b        |     c     |       d       
 ------------+-----------------+-----------+---------------
  1          | 1               | 1         | 1
  1          | 1               | 1         | 2
  3          | 2               | 2         | 5
 (3 rows)

The log file must contain SELECT statements, followed by the query
output in the standard Postgresql format, ie a set of named columns, a
separator line constructed of dashes and plus signs, and the rows,
followed by an "(N rows)" row count.  The SELECT statement must be
unambiguous, eg no embedded SQL keywords like INSERT, UPDATE, or
DELETE, and it must be terminated with a semicolon.  Normally, the
query output is sorted, but if the statement contains an ORDER BY
clause the query output for that query is not sorted,

=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2007 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

my $glob_id = "";

# optional set of prefixes to identify sql statements, query output,
# and sorted lines (for testing purposes)
#my $apref = 'a: ';
#my $bpref = 'b: ';
#my $cpref = 'c: ';
#my $dpref = 'S: ';
my $apref = '';
my $bpref = '';
my $cpref = '';
my $dpref = '';

BEGIN {
    my $man  = 0;
    my $help = 0;

    GetOptions(
               'help|?' => \$help, man => \$man, 
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

}

# convert a postgresql psql formatted table into an array of hashes
sub tablelizer
{
    my $ini = shift;

    # first, split into separate lines, the find all the column headings

    my @lines = split(/\n/, $ini);

    return undef
        unless (scalar(@lines));

    my $line1 = shift @lines;

    # look for <space>|<space>
    my @colheads = split(/\s+\|\s+/, $line1);

    # fixup first, last column head (remove leading,trailing spaces)

    $colheads[0] =~ s/^\s+//;
    $colheads[0] =~ s/\s+$//;
    $colheads[-1] =~ s/^\s+//;
    $colheads[-1] =~ s/\s+$//;

    return undef
        unless (scalar(@lines));
    
    shift @lines; # skip dashed separator

    my @rows;

    for my $lin (@lines)
    {
        my @cols = split(/\|/, $lin, scalar(@colheads));
        last 
            unless (scalar(@cols) == scalar(@colheads));

        my $rowh = {};

        for my $colhd (@colheads)
        {
            my $rawcol = shift @cols;

            $rawcol =~ s/^\s+//;
            $rawcol =~ s/\s+$//;

            $rowh->{$colhd} = $rawcol;
        }
        push @rows, $rowh;
    }

    return \@rows;
}




if (1)
{
    my $sql_statement = "";
    my @outarr;

    my $getrows = 0;
    my $getstatement = 0;
    my $has_order = 0;
    my $copy_select = 0;

  L_bigwhile:
    while (<>) # big while
    {
        my $ini = $_;

        if ($getrows) # getting rows from SELECT output
        {
            # special case for copy select
            if ($copy_select &&
                ($ini =~ m/(\-\-)|(ERROR)/))
            {
                my @ggg= sort @outarr;
                for my $line (@ggg)
                {
                    print $bpref, $line;
                }

                @outarr = ();
                $getrows = 0;
                $has_order = 0;
                $copy_select = 0;
                next;
            }


            # regex example: (5 rows)
            if ($ini =~ m/^\s*\(\d+\s+row(s)*\)\s*$/)
            {
                if ($has_order)
                {
                    for my $line (@outarr)
                    {
                        print $dpref, $line;
                    }
                }
                else
                {
                    my @ggg= sort @outarr;
                    for my $line (@ggg)
                    {
                        print $bpref, $line;
                    }
                }
                @outarr = ();
                $getrows = 0;
                $has_order = 0;
            }
        }
        else # finding SQL statement or start of SELECT output
        { 

            if ($ini =~ m/(insert|update|delete|select|\\d|copy)/i)
            {                
                $copy_select = 0;
                $has_order = 0;
                $sql_statement = "";
            }
            if ($ini =~ m/select/i)
            {
                $getstatement = 1;
            }
            if ($getstatement)
            {
                $sql_statement .= $ini;
            }
            if ($ini =~ m/\;/) # statement terminator
            {
                $getstatement = 0;
            }

            # prune notices with segment info if they are duplicates
#            if ($ini =~ m/^\s*(NOTICE|ERROR|HINT|DETAIL|WARNING)\:.*\s+\(seg.*pid.*\)/)
            if ($ini =~ m/^\s*(NOTICE|ERROR|HINT|DETAIL|WARNING)\:/)
            {
                $ini =~ s/\s+\(seg.*pid.*\)//;

                my $outsize = scalar(@outarr);

                my $lastguy = -1;

              L_checkfor:
                for my $jj (1..$outsize)
                {
                    my $checkstr = $outarr[$lastguy];

                    #remove trailing spaces for comparison
                    $checkstr =~ s/\s+$//;

                    my $skinny = $ini;
                    $skinny =~ s/\s+$//;

                    # stop when no more notices
                    last L_checkfor
                        if ($checkstr !~ m/^\s*(NOTICE|ERROR|HINT|DETAIL|WARNING)\:/);

                    # discard this line if matches a previous notice
                    if ($skinny eq $checkstr)
                    {
                        if (0) # debug code
                        {
                            $ini = "DUP: " . $ini;
                            last L_checkfor;
                        }
                        next L_bigwhile;
                    }
                    $lastguy--;
                }

            } # end if pruning notices


            # regex example: ---- or ---+---
            # need at least 3 dashes to avoid confusion with "--" comments
            if (($ini =~ m/^\s*((\-\-)(\-)+(\+(\-)+)*)+\s*$/)
                # special case for copy select
                || (($ini =~ m/^\s*copy\s*\(\s*select.*to stdout\;/i)
                    && ($ini !~ m/order\s+by/i)))
            { # sort this region

                # special case for copy select
                if ($ini =~ m/^\s*copy\s*\(\s*select.*to stdout\;/i)
                {
                    $copy_select = 1;
                    $sql_statement = "";
                }

                $getstatement = 0;

                for my $line (@outarr)
                {
                    print $apref, $line;
                }
                @outarr = ();
                
                print $apref, $ini;

                if (defined($sql_statement)
                    && length($sql_statement)
                    # multiline match 
                    && ($sql_statement =~ m/select.*order.*by/is))
                {
                    $has_order = 1; # so do *not* sort output

#                   $sql_statement =~ s/\n/ /gm;
#                   print "has order: ", $sql_statement, "\n";
                }
                else
                {
                    $has_order = 0; # need to sort query output

#                    $sql_statement =~ s/\n/ /gm;
#                    print "no order: ", $sql_statement, "\n";
                }
                $sql_statement = "";

                $getrows = 1;
                next;
            } # end sort this region
        } # end finding SQL

        # some cleanup of greenplum-specific messages
        if ($ini =~ m/\s+\(seg.*pid.*\)/)
        {
            $ini =~ s/\s+\(seg.*pid.*\)//;
        }

        if ($ini =~ m/WARNING:\s+foreign key constraint \".*\" will require costly sequential scans/)
        {
            $ini =~ s/\".*\"/\"dummy key\"/;
        }

        if ($ini =~ m/CONTEXT:.*\s+of this segment db input data/)
        {
            $ini =~ s/\s+of this segment db input data//;
        }

        # XXX XXX: note the discrepancy in the NOTICE messages
        # 'distributed by' vs 'DISTRIBUTED BY'
        if ($ini =~ m/NOTICE:\s+Table doesn\'t have \'distributed by\' clause\, and no column type is suitable/i)
        {
            next;
        }
        if ($ini =~ m/NOTICE:\s+Table doesn\'t have \'DISTRIBUTED BY\' clause/i)
        {
            next;
        }
        if ($ini =~ m/NOTICE:\s+Dropping a column that is part of the distribution policy/)
        {
            next;
        }
        if ($ini =~ m/NOTICE:\s+Table has parent\, setting distribution columns to match parent table/)
        {
            next;
        }
        if ($ini =~ m/HINT:\s+The \'DISTRIBUTED BY\' clause determines the distribution of data/)
        {
            next;
        }
        if ($ini =~ m/WARNING:\s+Referential integrity \(.*\) constraints are not supported in Greenplum Database/)
        {
            next;
        }



        if ($ini =~ m/^\s*Distributed by:\s+\(.*\)\s*$/)
        {
            next;
        }

        push @outarr, $ini;

    } # end big while

    for my $line (@outarr)
    {
        print $cpref, $line;
    }
}

exit();
