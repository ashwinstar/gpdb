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
have a fully-specified order.  The utility gpdiff.pl invokes atmsort
in order to compare the Greenplum test results against standard
Postgresql.

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

=head2 Advanced Usage 

atmsort supports several "commands" that allow finer-grained control
over the comparison process for SELECT queries.  These commands are
specified in comments in the following form:

 --
 -- order 1
 --
 SELECT a, b, c, d 
   from foo 
   ORDER BY 1;

or

 SELECT a, b, c, d 
   from foo 
   ORDER BY 1; -- order 1

The supported commands are:

=over 12

=item -- order column number[, column number...]

  The order directive is used to compare 
  "partially-ordered" query
  output.  The specified columns are assumed 
  to be ordered, and the  remaining columns are 
  sorted to allow for deterministic comparison.

=item -- ignore

  The ignore directive prefixes the SELECT output 
  with GP_IGNORE.  The diff command can use the -I flag 
  to ignore lines with this prefix.

=item -- mvd colnum[, colnum...] -> colnum[, colnum...] [; <additional specs>]

  mvd is designed to support Multi-Value Dependencies for 
  OLAP queries.  The syntax "col1,col2->col3,col4" indicates
  that the col1 and col2 values determine the col3, col4 
  result order.

=item -- start_ignore

  Ignore all results until the next "end_ignore" directive.
  The start_ignore directive prefixes all subsequent output 
  with GP_IGNORE, and all other formatting directives are 
  ignored as well.  The diff command can use the -I flag 
  to ignore lines with this prefix.

=item -- end_ignore

  Ends the ignored region that started with "start_ignore"

=back

Note that you can combine the directives for a single query, but each
directive must be on a separate line.  Multiple mvd specifications
must be on a single mvd line, separated by semicolons.  Note that 
start_ignore overrides all directives until the next end_ignore.


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

# reformat the query output according to the directive hash
sub format_query_output
{
    my ($has_order, $outarr, $directive) = @_;
    my $prefix = "";

    $directive = {} unless (defined($directive));

    $prefix = "GP_IGNORE:"
        if (exists($directive->{ignore}));

    if (exists($directive->{firstline}))
    {
        my $firstline = $directive->{firstline};
        my $ordercols = $directive->{order};
        my $mvdlist   = $directive->{mvd};

        # lines already have newline terminator, so just rejoin them.
        my $lines = join ("", @{$outarr});

        my $ah1 = tablelizer($lines, $firstline);

        unless (scalar(@{$ah1}))
        {
#            print "No tablelizer hash for $lines, $firstline\n";
#            print STDERR "No tablelizer hash for $lines, $firstline\n";

            return;
        }

        my @allcols = sort (keys(%{$ah1->[0]}));

        my @presortcols;
        if (defined($ordercols) && length($ordercols))
        {
#        $ordercols =~ s/^.*order\s*//;
            $ordercols =~ s/\n//gm;
            $ordercols =~ s/\s//gm;

            @presortcols = split(/\s*\,\s*/, $ordercols);
        }

        my @mvdcols;
        my @mvd_deps;
        my @mvd_nodeps;
        my @mvdspec;
        if (defined($mvdlist) && length($mvdlist))
        {
            $mvdlist  =~ s/\n//gm;
            $mvdlist  =~ s/\s//gm;

            # find all the mvd specifications (separated by semicolons)
            my @allspecs = split(/\;/, $mvdlist);

#            print "allspecs:", Data::Dumper->Dump(\@allspecs);

            for my $item (@allspecs)
            {
                my $realspec;
                # split the specification list, separating the
                # specification columns on the left hand side (LHS)
                # from the "dependent" columns on the right hand side (RHS)
                my @colset = split(/\-\>/, $item, 2);
                unless (scalar(@colset) == 2)
                {
                    print "invalid colset for $item\n";
                    print STDERR "invalid colset for $item\n";
                    next;
                }
                # specification columns (LHS)
                my @scols = split(/\,/, $colset[0]);
                unless (scalar(@scols))
                {
                    print "invalid dependency specification: $colset[0]\n";
                    print STDERR 
                        "invalid dependency specification: $colset[0]\n";
                    next;
                }
                # dependent columns (RHS)
                my @dcols = split(/\,/, $colset[1]);
                unless (scalar(@dcols))
                {
                    print "invalid specified dependency: $colset[1]\n";
                    print STDERR "invalid specified dependency: $colset[1]\n";
                    next;
                }
                $realspec = {};
                my $scol2 = [];
                my $dcol2 = [];
                my $sdcol = [];
                $realspec->{spec} = $item;
                push @{$scol2}, @scols;
                push @{$dcol2}, @dcols;
                push @{$sdcol}, @scols, @dcols;
                $realspec->{scol} = $scol2;
                $realspec->{dcol} = $dcol2;
                $realspec->{allcol} = $sdcol;

                push @mvdcols, @scols, @dcols;
                # find all the dependent columns
                push @mvd_deps, @dcols;
                push @mvdspec, $realspec;
            }

            # find all the mvd cols which are *not* dependent.  Need
            # to handle the case of self-dependency, eg "mvd 1->1", so
            # must build set of all columns, then strip out the
            # "dependent" cols.  So this is the set of all LHS columns
            # which are never on the RHS.
            my %get_nodeps;

            for my $col (@mvdcols)
            {
                $get_nodeps{$col} = 1;
            }

            # remove dependent cols
            for my $col (@mvd_deps)
            {
                if (exists($get_nodeps{$col}))
                {
                    delete $get_nodeps{$col};
                }
            }
            # now sorted and unique, with no dependents
            @mvd_nodeps = sort (keys(%get_nodeps));
#            print "mvdspec:", Data::Dumper->Dump(\@mvdspec);
#            print "mvd no deps:", Data::Dumper->Dump(\@mvd_nodeps);
        }

        my %unsorth;
        
        for my $col (@allcols)
        {
            $unsorth{$col} = 1;
        }
        for my $col (@presortcols)
        {
            if (exists($unsorth{$col}))
            {
                delete $unsorth{$col};
            }
        }
        for my $col (@mvdcols)
        {
            if (exists($unsorth{$col}))
            {
                delete $unsorth{$col};
            }
        }
        my @unsortcols = sort(keys(%unsorth));

#        print Data::Dumper->Dump([$ah1]);

        if (scalar(@presortcols))
        {
            my $hd1 = "sorted columns " . join(", ", @presortcols);

            print $hd1, "\n", "-"x(length($hd1)), "\n";

            for my $h_row (@{$ah1})
            {
                my @collist;

                @collist = ();

#            print "hrow:",Data::Dumper->Dump([$h_row]), "\n";            

                for my $col (@presortcols)
                {
#                print "col: ($col)\n";
                    if (exists($h_row->{$col}))
                    {
                        push @collist, $h_row->{$col};
                    }
                    else
                    {
                        my $maxcol = scalar(@allcols);
                        my $errstr = 
                            "specified ORDER column out of range: $col vs $maxcol\n";
                        print $errstr;
                        print STDERR $errstr;
                        last;
                    }
                }
                print $prefix, join(' | ', @collist), "\n";
            }
        }

        if (scalar(@mvdspec))
        {
            my @outi;

            my $hd1 = "multivalue dependency specifications";

            print $hd1, "\n", "-"x(length($hd1)), "\n";

            for my $mspec (@mvdspec)
            {
                $hd1 = $mspec->{spec};
                print $hd1, "\n", "-"x(length($hd1)), "\n";

                for my $h_row (@{$ah1})
                {
                    my @collist;

                    @collist = ();

#            print "hrow:",Data::Dumper->Dump([$h_row]), "\n";            

                    for my $col (@{$mspec->{allcol}})
                    {
#                print "col: ($col)\n";
                        if (exists($h_row->{$col}))
                        {
                            push @collist, $h_row->{$col};
                        }
                        else
                        {
                            my $maxcol = scalar(@allcols);
                            my $errstr = 
                            "specified MVD column out of range: $col vs $maxcol\n";
                            print $errstr;
                            print STDERR $errstr;
                            last;
                        }

                    }
                    push @outi, join(' | ', @collist);
                }
                my @ggg= sort @outi;

                for my $line (@ggg)
                {
                    print $prefix, $line, "\n";
                }
                @outi = ();
            }
        }
        my $hd2 = "unsorted columns " . join(", ", @unsortcols);

        # the "unsorted" comparison must include all columns which are
        # not sorted or part of an mvd specification, plus the sorted
        # columns, plus the non-dependent mvd columns which aren't
        # already in the list
        if ((scalar(@presortcols))
            || scalar(@mvd_nodeps))
        {
            if (scalar(@presortcols))
            {
                if (scalar(@mvd_deps))
                {
                    my %get_presort;

                    for my $col (@presortcols)
                    {
                        $get_presort{$col} = 1;
                    }
                    # remove "dependent" (RHS) columns
                    for my $col (@mvd_deps)
                    {
                        if (exists($get_presort{$col}))
                        {
                            delete $get_presort{$col};
                        }
                    }
                    # now sorted and unique, minus all mvd dependent cols
                    @presortcols = sort (keys(%get_presort));

                }

                if (scalar(@presortcols))
                {
                    $hd2 .= " ( " . join(", ", @presortcols) . ")";
                    # have to compare all columns as unsorted 
                    push @unsortcols, @presortcols;
                }
            }
            if (scalar(@mvd_nodeps))
            {
                my %get_nodeps;

                for my $col (@mvd_nodeps)
                {
                    $get_nodeps{$col} = 1;
                }
                # remove "nodeps" which are already in the output list
                for my $col (@unsortcols)
                {
                    if (exists($get_nodeps{$col}))
                    {
                        delete $get_nodeps{$col};
                    }
                }
                # now sorted and unique, minus all unsorted/sorted cols
                @mvd_nodeps = sort (keys(%get_nodeps));
                if (scalar(@mvd_nodeps))
                {
                    $hd2 .= " (( " . join(", ", @mvd_nodeps) . "))";
                    # have to compare all columns as unsorted 
                    push @unsortcols, @mvd_nodeps;
                }

            }
            
        }

        print $hd2, "\n", "-"x(length($hd2)), "\n";

        my @finalunsort;

        if (scalar(@unsortcols))
        {
            for my $h_row (@{$ah1})
            {
                my @collist;

                @collist = ();

                for my $col (@unsortcols)
                {
                    if (exists($h_row->{$col}))
                    {
                        push @collist, $h_row->{$col};
                    }
                    else
                    {
                        my $maxcol = scalar(@allcols);
                        my $errstr = 
                            "specified UNSORT column out of range: $col vs $maxcol\n";
                        print $errstr;
                        print STDERR $errstr;
                        last;
                    }

                }
                push @finalunsort, join(' | ', @collist);
            }
            my @ggg= sort @finalunsort;

            for my $line (@ggg)
            {
                print $prefix, $line, "\n";
            }
        }

        return;
    } # end order


    if ($has_order)
    {
        for my $line (@{$outarr})
        {
            print $dpref, $prefix, $line;
        }
    }
    else
    {
        my @ggg= sort @{$outarr};
        for my $line (@ggg)
        {
            print $bpref, $prefix, $line;
        }
    }


}


if (1)
{
    my $sql_statement = "";
    my @outarr;

    my $getrows = 0;
    my $getstatement = 0;
    my $has_order = 0;
    my $copy_select = 0;
    my $directive = {};
    my $big_ignore = 0;
    my $error_detail_exttab_trifecta_skip = 0; # don't ask!

  L_bigwhile:
    while (<>) # big while
    {
        my $ini = $_;

        if ($error_detail_exttab_trifecta_skip)
        {
            $error_detail_exttab_trifecta_skip = 0;
            next;
        }

        if ($big_ignore)
        {
            if ($ini =~ m/\-\-\s*end\_ignore\s*$/i)
            { 
                $big_ignore = 0;
            }
            print "GP_IGNORE:", $ini;
            next;
        }

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

                format_query_output($has_order, \@outarr, $directive);

                $directive = {};
                @outarr = ();
                $getrows = 0;
                $has_order = 0;
            }
        }
        else # finding SQL statement or start of SELECT output
        { 
            if ($ini =~ m/\-\-\s*start\_ignore\s*$/i)
            { 
                $big_ignore = 1;

                for my $line (@outarr)
                {
                    print $apref, $line;
                }
                @outarr = ();

                print "GP_IGNORE:", $ini;
                next;
            }

            if ($ini =~ m/(insert|update|delete|select|\\d|copy)/i)
            {                
                $copy_select = 0;
                $has_order = 0;
                $sql_statement = "";
            }

            if ($ini =~ m/\-\-\s*ignore\s*$/i)
            { 
                $directive->{ignore} = "ignore";
            }
            if ($ini =~ m/\-\-\s*order\s+\d+.*$/i)
            { 
                my $olist = $ini;
                $olist =~ s/^.*\-\-\s*order//;
                $directive->{order} = $olist;
            }
            if ($ini =~ m/\-\-\s*mvd\s+\d+.*$/i)
            { 
                my $olist = $ini;
                $olist =~ s/^.*\-\-\s*mvd//;
                $directive->{mvd} = $olist;
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
                } # end for



            } # end if pruning notices

            # MPP-1492 allow:
            #  copy (select ...) to stdout
            #  \copy (select ...) to stdout
            # and special case these guys:
            #  copy test1 to stdout
            #  \copy test1 to stdout
            my $copysel_regex = 
            '^\s*((.copy.*test1.*to stdout)|(copy.*test1.*to stdout\;))';

            # regex example: ---- or ---+---
            # need at least 3 dashes to avoid confusion with "--" comments
            if (($ini =~ m/^\s*((\-\-)(\-)+(\+(\-)+)*)+\s*$/)
                # special case for copy select
                || (($ini =~ m/$copysel_regex/i)
                    && ($ini !~ m/order\s+by/i)))
            { # sort this region

                if (exists($directive->{order}) ||
                    exists($directive->{mvd}))
                {
                    $directive->{firstline} = $outarr[-1];
                }

                # special case for copy select
                if ($ini =~ m/$copysel_regex/i)
                {
#                    print "copy select: $ini\n";
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

        # distributed transactions
        if ($ini =~ m/(ERROR|WARNING|CONTEXT|NOTICE):.*gid\s+=\s+(\d+)/)
        {
            $ini =~ s/gid.*/gid DUMMY/;
        }
        if ($ini =~ m/(ERROR|WARNING|CONTEXT|NOTICE):.*DTM error.*gathered (\d+) results from cmd.*/)
        {
            $ini =~ s/gathered.*results/gathered SOME_NUMBER_OF results/;
        }
        if ($ini =~ m/(ERROR|WARNING|CONTEXT|NOTICE):.*connection.*failed.*http/)
        {
            $ini =~ s|connection.*failed.*http.*|connection failed http://DUMMY_LOCATION|;
        }
        if ($ini =~ m/External table .*line (\d)+/)
        {
            $ini =~ s/External table .*line (\d)+.*/External table DUMMY_EX, line DUMMY_LINE of DUMMY_LOCATION/;
              $ini =~ s/\s+/ /;
             # MPP-1557,AUTO-3: horrific ERROR DETAIL External Table trifecta
             if ($ini !~ m/^DETAIL/)
             {
                # find a "blank" DETAIL tag preceding current line
                if (scalar(@outarr) && ($outarr[-1] =~ m/^DETAIL:\s+$/))
                {
                   pop @outarr;
                   $ini = "DETAIL: " . $ini;
                   $ini =~ s/\s+/ /;
                   # need to skip the next record
                   $error_detail_exttab_trifecta_skip = 1;
                }
             }
             if (scalar(@outarr) && 
                 ($outarr[-1] =~ m/^ERROR:\s+missing\s+data\s+for\s+column/))
             {
                $outarr[-1] = "ERROR:  missing data for column DUMMY_COL\n";
             }

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
