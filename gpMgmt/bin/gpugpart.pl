#!/usr/bin/perl
#
# $Header: /Users/jcohen/perl/RCS/explain.pl,v 1.3 2006/11/06 23:32:06 jcohen Exp jcohen $
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

B<atmsort.pl> - [A] [T]est [M]echanism Sort: sort the contents of SQL log files to aid diff comparison

=head1 SYNOPSIS

B<atmsort.pl> [options] logfile [logfile...]

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


=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2008 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

my $glob_id = "";
my $glob_connect;
my $glob_dosql;

BEGIN {
    my $man  = 0;
    my $help = 0;
    my $conn;
    my $dosql;

    GetOptions(
               'help|?' => \$help, man => \$man, 
               "connect=s" => \$conn,
               "sql" => \$dosql
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    $glob_connect = $conn;

    $glob_connect = '-p 11000 template1'
        unless (defined($glob_connect));

    $glob_dosql = $dosql;

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


sub infix_proc_fn
{
    my ($biga, $rex) = @_;

    my $outi = [];
                        
    while (scalar(@{$biga}))
    {
        my $curr1 = shift @{$biga};

        $curr1 = paren_fixup($curr1);

        if ($curr1 =~ m/$rex/)
        {
            my $h1 = {};

            $h1->{val} = $curr1;
            $h1->{left} = pop @{$outi};

            my $curr2 = shift @{$biga};
            $curr2 = paren_fixup($curr2);

            $h1->{right} = $curr2;

            push @{$outi}, $h1;                                
        }
        else
        {
            push @{$outi}, $curr1;
        }
    }

    return $outi;
}

sub infix_proc
{
    my $biga = shift;

    my @f2;

    for my $vv3 (@{$biga})
    {
        push @f2, $vv3
            if  (defined($vv3) && length($vv3) && ($vv3 !~ m/^\s+$/));
    }

    my $biga2 = infix_proc_fn(\@f2, qr/^(\<|\>|\=|\<\>|\<\=|\>\=|\+|\-|\%|\/)$/ );
    my $biga3 = infix_proc_fn($biga2, qr/^AND$/i );
    my $biga4 = infix_proc_fn($biga3, qr/^OR$/i );

    return $biga4;
}

sub paren_fixup
{
    my $foo = shift;
    
    return $foo
        unless ((ref($foo) eq 'HASH') && (exists($foo->{biga})));

    my $biga = $foo->{biga};

    my $biga2 = infix_proc($biga);

    # reduced to a single val
    return $biga2->[0]
        if (1 == scalar(@{$biga2}));

    $foo->{biga} = $biga2;

    return $foo;
}

sub paren_proc_fn
{
    my ($biga, $level) = @_;

    my $outi = [];

    while (scalar(@{$biga}))
    {
        my $tok = shift @{$biga};

        if (defined($tok))
        {
            return $outi
                if ($tok =~ m/^\)$/);

            if ($tok =~ m/^\($/)
            {
                my $foo  = paren_proc_fn($biga, $level + 1);

                $tok = {};
                $tok->{biga} = $foo;
            }
        }

        push @{$outi}, $tok;
    }

    return $outi;
}

sub paren_proc
{
    my $biga = shift;

    my $biga2 = [];

    for my $vv3 (@{$biga})
    {
        push @{$biga2}, $vv3
            if  (defined($vv3) && length($vv3) && ($vv3 !~ m/^\s+$/));
    }


    my $outi = paren_proc_fn($biga2, 0);

    return $outi;
}

sub pw1
{
    my $txt = shift;
                    
    use Text::ParseWords;

    $txt =~ s/^\s+//;
    $txt =~ s/\s+$//;

    my @wfoo = parse_line('[()\s+]', 'delimiters', $txt);
#                        my @wfoo = parse_line('\s+', 0, $txt);
#    print Data::Dumper->Dump(\@wfoo);


    my $biga  = paren_proc(\@wfoo);
    my $biga2 = infix_proc($biga);

    # reduced to a single val
    return $biga2->[0]
        if (1 == scalar(@{$biga2}));

    return $biga2;
}

sub classify_range_merge
{
    my ($spec1, $spec2) = @_;

    my $npartSpec = {};
    my $specstart = {};
    my $specend   = {};
    
    $npartSpec->{colh} = {};
    $npartSpec->{oper} = "RANGE";
    $npartSpec->{typ}  = "RANGE";

    $specstart->{colh} = {};
    $specstart->{colh_oper} = {};
    $specstart->{oper} = ();
    
    $specend->{colh} = {};
    $specstart->{colh_oper} = {};
    $specend->{oper} = ();

    $npartSpec->{range_start} = $specstart;
    $npartSpec->{range_end}   = $specend;

#    print "merging...\n";

    # build a copy of the range_start of spec1

    while ( my ($kk, $vv) = each(%{$spec1->{range_start}->{colh}}))
    {
        $specstart->{colh}->{$kk} = []
            unless (exists($specstart->{colh}->{$kk}));
        $specstart->{colh_oper}->{$kk} = []
            unless (exists($specstart->{colh_oper}->{$kk}));

        push (@{$specstart->{colh}->{$kk}}, @{$vv});
        push (@{$specstart->{colh_oper}->{$kk}}, 
              @{$spec1->{range_start}->{colh_oper}->{$kk}});
    }        

    # merge in the range_start from spec2

    while ( my ($kk, $vv) = each(%{$spec2->{range_start}->{colh}}))
    {
        if (exists($specstart->{colh}->{$kk}))
        {
            print STDERR "duplicate range start specification for:\n",
            Data::Dumper->Dump([$spec1]), "\n",
            Data::Dumper->Dump([$spec2]), "\n";
            return undef;
        }
        $specstart->{colh}->{$kk} = [];
        $specstart->{colh_oper}->{$kk} = [];

        push (@{$specstart->{colh}->{$kk}}, @{$vv});
        push (@{$specstart->{colh_oper}->{$kk}}, 
              @{$spec2->{range_start}->{colh_oper}->{$kk}});
    }        

    # build a copy of the range_end of spec1

    while ( my ($kk, $vv) = each(%{$spec1->{range_end}->{colh}}))
    {
        $specend->{colh}->{$kk} = []
            unless (exists($specend->{colh}->{$kk}));
        $specend->{colh_oper}->{$kk} = []
            unless (exists($specend->{colh_oper}->{$kk}));

        push (@{$specend->{colh}->{$kk}}, @{$vv});
        push (@{$specend->{colh_oper}->{$kk}}, 
              @{$spec1->{range_end}->{colh_oper}->{$kk}});
    }        

    # merge in the range_end from spec2

    while ( my ($kk, $vv) = each(%{$spec2->{range_end}->{colh}}))
    {
        if (exists($specend->{colh}->{$kk}))
        {
            print STDERR "duplicate range end specification for:\n",
            Data::Dumper->Dump([$spec1]), "\n",
            Data::Dumper->Dump([$spec2]), "\n";
            return undef;
        }
        $specend->{colh}->{$kk} = [];
        $specend->{colh_oper}->{$kk} = [];

        push (@{$specend->{colh}->{$kk}}, @{$vv});
        push (@{$specend->{colh_oper}->{$kk}}, 
              @{$spec2->{range_end}->{colh_oper}->{$kk}});
    }        


    return $npartSpec;

} # end classify_range_merge


sub classify_range_fixup
{
    my $partSpec = shift;

    my $npartSpec = {};
    my $specstart = {};
    my $specend   = {};
    
    $npartSpec->{colh} = {};
    $npartSpec->{oper} = "RANGE";
    $npartSpec->{typ}  = "RANGE";

    $specstart->{colh} = {};
    $specstart->{colh_oper} = {};
    $specstart->{oper} = ();
    
    $specend->{colh} = {};
    $specstart->{colh_oper} = {};
    $specend->{oper} = ();

    $npartSpec->{range_start} = $specstart;
    $npartSpec->{range_end}   = $specend;
    
    if ($partSpec->{typ} =~ m/RANGE_START/)
    {
        while ( my ($kk, $vv) = each(%{$partSpec->{colh}}))
        {
            $specstart->{colh}->{$kk} = []
                unless (exists($specstart->{colh}->{$kk}));
            $specstart->{colh_oper}->{$kk} = []
                unless (exists($specstart->{colh_oper}->{$kk}));
            push (@{$specstart->{colh}->{$kk}}, @{$vv});

            unless (1 == scalar(@{$vv}))
            {
                print STDERR "bad range start\n";
                return undef;
            }

            push (@{$specstart->{colh_oper}->{$kk}}, $partSpec->{oper});
        }        

    }
    elsif ($partSpec->{typ} =~ m/RANGE_END/)
    {
        while ( my ($kk, $vv) = each(%{$partSpec->{colh}}))
        {
            $specend->{colh}->{$kk} = []
                unless (exists($specend->{colh}->{$kk}));
            $specend->{colh_oper}->{$kk} = []
                unless (exists($specend->{colh_oper}->{$kk}));
            push (@{$specend->{colh}->{$kk}}, @{$vv});

            unless (1 == scalar(@{$vv}))
            {
                print STDERR "bad range end\n";
                return undef;
            }

            push (@{$specend->{colh_oper}->{$kk}}, $partSpec->{oper});
        }        

    }
    else
    {
        print STDERR "bad range spec\n",
        Data::Dumper->Dump([$partSpec]);
        return undef;
    }

    return $npartSpec;

} # end classify_range_fixup

sub classify_and
{
    my ($node, $partSpec) = @_;

    if ($node->{val} =~ m/^AND$/i)
    {
        # compound list
        unless (exists($node->{left})
                && exists($node->{left}->{partSpec})
                && exists($node->{right})
                && exists($node->{right}->{partSpec}))
        {
            return undef;
        }

        my $left  = $node->{left};
        my $right = $node->{right};

        # AND of LIST must match
        unless (exists($left->{partSpec}->{typ})
                && exists($right->{partSpec}->{typ}))
        {
            return undef;
        }

        if ($left->{partSpec}->{typ} ne
            $right->{partSpec}->{typ})
        {
            print STDERR "cannot match types for \n",
            Data::Dumper->Dump([$left->{partSpec}]),
            "\n",
            Data::Dumper->Dump([$right->{partSpec}]),
            "\n";                    
            return undef;
        }

        if ($left->{partSpec}->{typ} eq "LIST")
        {
            $partSpec->{typ} = "LIST";
            $partSpec->{oper} = "LIST";

            while ( my ($kk, $vv) = each(%{$left->{partSpec}->{colh}}))
            {
                $partSpec->{colh}->{$kk} = []
                    unless (exists($partSpec->{colh}->{$kk}));
                push (@{$partSpec->{colh}->{$kk}}, @{$vv});
            }        

            while ( my ($kk, $vv) = each(%{$right->{partSpec}->{colh}}))
            {
                $partSpec->{colh}->{$kk} = []
                    unless (exists($partSpec->{colh}->{$kk}));
                push (@{$partSpec->{colh}->{$kk}}, @{$vv});
            }        

            # XXX XXX:
            delete $left->{partSpec};
            delete $right->{partSpec};

            $node->{partSpec} = $partSpec;

        }
        elsif ($left->{partSpec}->{typ} eq "RANGE")
        {
            $partSpec = 
                classify_range_merge(
                                     $left->{partSpec},
                                     $right->{partSpec}
                                     );

            # XXX XXX:
            delete $left->{partSpec};
            delete $right->{partSpec};

            $node->{partSpec} = $partSpec;
        }
        else
        {
            # XXX XXX: fixme
            return undef;

        }


    } # end AND

    return $partSpec;
}

sub classify_or
{
    my ($node, $partSpec) = @_;

    if ($node->{val} =~ m/^OR$/i)
    {
        # compound list
        unless (exists($node->{left})
                && exists($node->{left}->{partSpec})
                && exists($node->{right})
                && exists($node->{right}->{partSpec}))
        {
            return undef;
        }

        # OR of LIST must match
        unless (exists($node->{left}->{partSpec}->{typ})
                && exists($node->{right}->{partSpec}->{typ}))
        {
            return undef;
        }

        my $left  = $node->{left};
        my $right = $node->{right};

        if ($left->{partSpec}->{typ} ne
            $right->{partSpec}->{typ})
        {
            print STDERR "cannot match types for \n",
            Data::Dumper->Dump([$left->{partSpec}]),
            "\n",
            Data::Dumper->Dump([$right->{partSpec}]),
            "\n";                    
            return undef;
        }
                
        if ($left->{partSpec}->{typ} ne "LIST")
        {
            print STDERR "cannot OR non-LIST types \n",
            Data::Dumper->Dump([$left->{partSpec}]),
            "\n",
            Data::Dumper->Dump([$right->{partSpec}]),
            "\n";                    
            return undef;
        }

        my @l1 = sort(keys(%{$left->{partSpec}->{colh}}));
        my @r1 = sort(keys(%{$right->{partSpec}->{colh}}));

        if (scalar(@l1) != scalar(@r1))
        {
            print STDERR "column number disparity for \n",
            Data::Dumper->Dump([$left->{partSpec}]),
            "\n",
            Data::Dumper->Dump([$right->{partSpec}]),
            "\n";                    
            return undef;
        }
                
        for my $col1 (@l1)
        {
            my $col2 = shift @r1;
            
            if ($col1 ne $col2)
            {
                print STDERR "column name disparity for \n",
                Data::Dumper->Dump([$left->{partSpec}]),
                "\n",
                Data::Dumper->Dump([$right->{partSpec}]),
                "\n";                    
                return undef;
            }

        } # end for

        $partSpec->{typ} = "LIST";
        $partSpec->{oper} = "LIST";

        while ( my ($kk, $vv) = each(%{$left->{partSpec}->{colh}}))
        {
            $partSpec->{colh}->{$kk} = []
                unless (exists($partSpec->{colh}->{$kk}));
            push (@{$partSpec->{colh}->{$kk}}, @{$vv});
        }        

        while ( my ($kk, $vv) = each(%{$right->{partSpec}->{colh}}))
        {
            $partSpec->{colh}->{$kk} = []
                unless (exists($partSpec->{colh}->{$kk}));
            push (@{$partSpec->{colh}->{$kk}}, @{$vv});
        }        

        # XXX XXX:
        delete $left->{partSpec};
        delete $right->{partSpec};

        $node->{partSpec} = $partSpec;

    } # end OR

    return $partSpec;
}

sub classify_hash
{
    my ($node, $partSpec) = @_;

    $partSpec->{typ} = "HASH";

    $partSpec->{hash_cols} = [];
    $partSpec->{hash_num} = -1;
    $partSpec->{hash_max} = -1;

    my @foo = 
        ($node->{where} =~ 
         #                cols      %    maxn    +   maxn    =   hash # 
         m/^\(\(hashtext\((.*)\)\s+\%\s+(\d+)\s+\+\s+(\d+).*\=\s+(\d+)/i);

    
    if (scalar(@foo) != 4)
    {
        print STDERR "bad hashtext function: ", $node->{where}, "\n";
        return undef;
    }

#    print Data::Dumper->Dump(\@foo);

    my $allcols = shift @foo;

    $partSpec->{hash_num} = pop @foo;
    $partSpec->{hash_max} = pop @foo;

    my @cols = split(/\|\|/, $allcols);

    for my $c1 (@cols)
    {
        $c1 =~ s/\:\:text//gm;
        $c1 =~ s/^\s*new\.//gm;
        $c1 =~ s/^\s+//gm;
        $c1 =~ s/\s+$//gm;

        push @{$partSpec->{hash_cols}}, $c1;
    }


    $node->{partSpec} = $partSpec;    

    return $partSpec;
} # end classify_hash

sub classify_fn
{
    my ($node, $partSpec) = @_;

    return undef
        unless ((ref($node) eq 'HASH') && exists($node->{val}));

#    print "classify_fn: ", Data::Dumper->Dump([$node]);

    if (exists($node->{left})
        && (ref($node->{left}) eq 'HASH')
        && (!exists($node->{left}->{partSpec})))
    {

        my $lpartSpec = {};

        $lpartSpec->{colh} = {};
        $lpartSpec->{oper} = ();

        $lpartSpec = classify_fn($node->{left}, $lpartSpec);
    }
    if (exists($node->{right})
        && (ref($node->{right}) eq 'HASH')
        && (!exists($node->{right}->{partSpec})))
    {

        my $lpartSpec = {};

        $lpartSpec->{colh} = {};
        $lpartSpec->{oper} = ();

        $lpartSpec = classify_fn($node->{right}, $lpartSpec);
    }
    

    if ($node->{val} =~ m/^(\<|\>|\=|\<\>|\<\=|\>\=)$/)
    {
        # simple list

        my @foo = ($node->{val} =~ m/^(\<|\>|\=|\<\>|\<\=|\>\=)$/ );

        my $oper = $foo[0];
        
        my ($colname, $colval);

        if ($node->{left} =~ m/^new\./)
        {
            $colname = $node->{left};
            $colval  = $node->{right};
        }
        elsif ($node->{right} =~ m/^new\./)
        {
            $colname = $node->{right};
            $colval  = $node->{left};

            # normalize oper for colname on left
            if ($colname !~ m/^\<\>/)
            {
                if ($colname =~ m/^\</)
                {
                    $oper =~ s/^\</\>/;
                }
                elsif ($colname =~ m/^\>/)
                {
                    $oper =~ s/^\>/\</;
                }
            }

        }
        else
        {
            print STDERR "no colname!!!\n\n";
            $partSpec->{typ} = "UNKNOWN";
            return $partSpec;
        }

        $colname =~ s/^new\.//;

        $partSpec->{colh}->{$colname} = []
            unless (exists($partSpec->{colh}->{$colname}));


        push @{$partSpec->{colh}->{$colname}}, $colval;

        $partSpec->{oper} = $oper;

        if ($oper =~ m/^\=$/)
        {
            $partSpec->{typ} = "LIST";
        }
        elsif ($oper =~ m/^(\>|\>\=)$/ )
        {
            $partSpec->{typ} = "RANGE_START";
        }
        elsif ($oper =~ m/^(\<|\<\=)$/ )
        {
            $partSpec->{typ} = "RANGE_END";
        }
        else
        {
            $partSpec->{typ} = "UNKNOWN";
        }

        $partSpec = classify_range_fixup($partSpec)
            if ($partSpec->{typ} =~ m/^RANGE/);

        $node->{partSpec} = $partSpec;
    }


    if ($node->{val} =~ m/^OR$/i)
    {
        $partSpec = classify_or($node, $partSpec);
    }

    if ($node->{val} =~ m/^AND$/i)
    {
        $partSpec = classify_and($node, $partSpec);
    }


    return $partSpec;
}

sub classify
{
    my $node = shift;

    return 0
        unless (exists($node->{parsetree}));

    my $partSpec = {};

    $partSpec->{colh} = {};
    $partSpec->{oper} = ();

    if ($node->{where} =~ m/^\(\(hashtext\(.*\)\s+\%\s+/i)
    {
        $partSpec = classify_hash($node, $partSpec);
        $node->{parsetree}->{partSpec} = $partSpec;
    }
    else
    {
        $partSpec = classify_fn($node->{parsetree}, $partSpec);
    }
#    print Data::Dumper->Dump([$partSpec]);
}

sub prettyprintPartSpec
{
    my $partSpec = shift;

    if ($partSpec->{typ} eq "HASH")
    {
        print "  Columns: (", 
        join(", ", @{$partSpec->{hash_cols}}), ")\n";
        print "  Partition Number ", $partSpec->{hash_num},
        " of ", $partSpec->{hash_max}, "\n";
    }
    elsif ($partSpec->{typ} eq "LIST")
    {
        print "  Columns: (", 
        join(", ", sort(keys(%{$partSpec->{colh}}))), ")\n";

        my @foo = sort(keys(%{$partSpec->{colh}}));
        my $maxn = scalar(@{$partSpec->{colh}->{$foo[0]}});

        my @vals;

        for my $ii (0..($maxn-1))
        {
            for my $coln (@foo)
            {
                push @vals, $partSpec->{colh}->{$coln}->[$ii];
            }
        }
        print "  Values: (", 
        join(", ", @vals), ")\n";

    }
    elsif ($partSpec->{typ} eq "RANGE")
    {
        my @rt = qw(range_start range_end);

        for my $rtv (@rt)
        {
            if ($rtv =~ m/start/)
            {
                print "  Start: \n";
            }
            else
            {
                print "  End: \n";
            }
        
            my $pspec = $partSpec->{$rtv};
            my @foo = sort(keys(%{$pspec->{colh}}));
            my $maxn = scalar(@{$pspec->{colh}->{$foo[0]}});

            for my $cnam (@foo)
            {
                print "   ", $cnam, " ", $pspec->{colh_oper}->{$cnam}->[0],
                " ",
                $pspec->{colh}->{$cnam}->[0], "\n";
            }

        }
        

    }
}

sub prettydump
{
    my $reg_tabs = shift;

    my @tabnames = sort(keys(%{$reg_tabs}));

    for my $kk (@tabnames)
    {
        my $vv = $reg_tabs->{$kk};

        next 
            unless (exists($vv->{rules}));

        print "Table: $kk\n";

        if (exists($vv->{rules}))
        {
            my @r1 = sort(keys(%{$vv->{rules}}));

            for my $kk2 (@r1)
            {
                my $vv2 = $vv->{rules}->{$kk2};

                next
                    unless (exists($vv2->{parsetree})
                            && exists($vv2->{parsetree}->{partSpec}));

                print "  Child: ", $vv2->{child}, "\n";

                my $partSpec = $vv2->{parsetree}->{partSpec};
                
                print "  Partition Type: ", $partSpec->{typ}, "\n";
                prettyprintPartSpec($partSpec);                
                            
            }
        }

    }
} # end prettydump

sub cat_init_tpart
{
    my ($t_part, $t_pcols, $tablename, $partSpec, $partype) = @_;

    my $hpart = {};

    $hpart->{partype} = $partype;
    $t_part->{$tablename} = $hpart;

    my $acols = [];

    if ($partSpec->{typ} eq "HASH")
    {
        for my $c1 (@{$partSpec->{hash_cols}})
        {
            push @{$acols}, $c1;
        }
        $hpart->{parmax} = $partSpec->{hash_max};
    }
    elsif ($partSpec->{typ} eq "LIST")
    {
        my @foo = sort(keys(%{$partSpec->{colh}}));
        
        for my $c1 (@foo)
        {
            push @{$acols}, $c1;
        }
    }
    elsif ($partSpec->{typ} eq "RANGE")
    {
        my %bothkeys;

        my @rt = qw(range_start range_end);
        
        for my $rtv (@rt)
        {
            my $pspec = $partSpec->{$rtv};
            my @foo = sort(keys(%{$pspec->{colh}}));
            
            for my $cnam (@foo)
            {
                $bothkeys{$cnam} = 1;
            }

        }
        my @foo = sort(keys(%bothkeys));
                        
        for my $c1 (@foo)
        {
            push @{$acols}, $c1;
        }
    }

    $t_pcols->{$tablename} = $acols;
    
    $hpart->{natts} =  scalar(@{$acols});
    
} # end sub cat_init_tpart

sub cat_boundspec 
{
    my ($t_bspec, $t_ritem, $childname, $partSpec, $partype, $rule_h) = @_;

    my $bs_stat = [];

    my $childpack = $childname;

    # quurl - url quoting
    $childpack =~ s/([^a-zA-Z0-9])/uc(sprintf("%%%02lx",  ord $1))/eg;

    # unquurl - $str =~ s/\%([A-Fa-f0-9]{2})/pack('C', hex($1))/seg;

    if ($partSpec->{typ} eq "HASH")
    {
        my $bskey = $childpack . ",HASH";

        $t_bspec->{$bskey} = {};

        $t_bspec->{$bskey}->{numritems} = 1;
        $t_bspec->{$bskey}->{bsnatts} = 1;

        my $rikey = $bskey . ",1,1";
        $t_ritem->{$rikey} = {};
        $t_ritem->{$rikey}->{colval} = $partSpec->{hash_num};
    }
    elsif ($partSpec->{typ} eq "LIST")
    {
        my $bskey = $childpack . ",LIST";

        $t_bspec->{$bskey} = {};

        my @foo = sort(keys(%{$partSpec->{colh}}));
        my $maxn = scalar(@{$partSpec->{colh}->{$foo[0]}});

        $t_bspec->{$bskey}->{bsnatts} = $maxn;

        my @vals;

        for my $ii (0..($maxn-1))
        {
            for my $coln (@foo)
            {
                push @vals, $partSpec->{colh}->{$coln}->[$ii];
            }
        }

        $t_bspec->{$bskey}->{numritems} = scalar(@vals)/$maxn;        


        my $ricolpos = 1;
        my $rinum    = 1;
        my $rikey = $bskey . "," . $rinum . "," . $ricolpos;

        for my $vv (@vals)
        {
            $t_ritem->{$rikey} = {};

            $t_ritem->{$rikey}->{colval} = $vv;

            $ricolpos++;

            if ($ricolpos > $maxn)
            {
                $ricolpos = 1;
                $rinum++;
            }

            $rikey = $bskey . "," . $rinum . "," . $ricolpos;

        }

    }
    elsif ($partSpec->{typ} eq "RANGE")
    {
        my @rt = qw(range_start range_end);

        for my $rtv (@rt)
        {
            my $pspec = $partSpec->{$rtv};
            my @foo = sort(keys(%{$pspec->{colh}}));
            my $maxn = scalar(@{$pspec->{colh}->{$foo[0]}});

            my $bskey = $childpack;

            if ($rtv =~ m/start/)
            {
                $bskey .= ",START";
            }
            else
            {
                $bskey .= ",END";
            }
        
            my @baz;

            my $ricolpos = 1;
            my $rinum    = 1;
            my $rikey = $bskey . "," . $rinum . "," . $ricolpos;


            for my $cnam (@foo)
            {
                my $ss1 = $cnam . " " . $pspec->{colh_oper}->{$cnam}->[0]
                    . " " . $pspec->{colh}->{$cnam}->[0];

                $t_ritem->{$rikey} = {};

                $t_ritem->{$rikey}->{colval} = $pspec->{colh}->{$cnam}->[0];
                
                $ricolpos++;

                if ($ricolpos > $maxn)
                {
                    $ricolpos = 1;
                    $rinum++;
                }

                $rikey = $bskey . "," . $rinum . "," . $ricolpos;

                push @baz, $ss1;
            }

            push @{$bs_stat}, join(" AND ", @baz);
        }
        

    }

    return $bs_stat;


} # end cat_boundspec 

sub _stringfix
{
    my $str = shift;

    if (defined($str))
    {
        my $foo = $str;
        $foo =~ s/\'/\'\'/gm;

        return '\'' . $foo . '\'';
    }
    else
    {
        return "NULL";
    }

}

sub do_insert_partition_table
{
    my $t_part  = shift; # partition table

    my @foo = sort(keys(%{$t_part}));

    print "\n";

    for my $kk (@foo)
    {
        my $vv = $t_part->{$kk};

        my ($partype, $parparent, $parmax, $parnatts, $parrelname);
        
        $partype = _stringfix($vv->{partype}); 
        $parparent = _stringfix($vv->{parparent}); 
        $parmax = $vv->{parmax} || 0;
        $parnatts = $vv->{natts};
        $parrelname = _stringfix($kk);

        print "INSERT INTO jc_partition values($parrelname, $partype, $parparent, $parmax, $parnatts);\n";

    }
} # end  do_insert_partition_table

sub do_insert_part_cols
{
    my $t_pcols = shift; # part_cols

    my @foo = sort(keys(%{$t_pcols}));

    print "\n";

    for my $kk (@foo)
    {
        my $vv = $t_pcols->{$kk};

        my $colpos = 1;

        for my $cname (@{$vv})
        {
            my $parrelname = _stringfix($kk);
            my $colname = _stringfix($cname);

            print 
                "INSERT INTO jc_part_cols values($parrelname, $colpos, $colname);\n";
            $colpos++;
        }

    }

} # end  do_insert_part_cols
sub do_insert_part_boundspec
{
    my $t_bspec = shift; # part_boundspec

    my @foo = sort(keys(%{$t_bspec}));

    print "\n";

    for my $kk (@foo)
    {
        my $vv = $t_bspec->{$kk};
        my @ggg = split(/\,/, $kk);
        
        my $pbschild = shift (@ggg);
        my $pbstype  = shift (@ggg);
        $pbschild =~ s/\%([A-Fa-f0-9]{2})/pack('C', hex($1))/seg;
        $pbschild = _stringfix($pbschild);
        $pbstype  = _stringfix($pbstype);

        my $pbsisinclusive = "NULL"; # XXX XXX: FIXME

        my $bsnatts = $vv->{bsnatts};
        my $numritems = $vv->{numritems};

        print "INSERT INTO jc_part_boundspec values($pbschild, $pbstype, $pbsisinclusive, $bsnatts, $numritems);\n";

    }
} # end  do_insert_part_boundspec
sub do_insert_part_rangeitem
{
    my $t_ritem = shift; # rangeitem

    my @foo = sort(keys(%{$t_ritem}));

    print "\n";

    for my $kk (@foo)
    {
        my $vv = $t_ritem->{$kk};
        my @ggg = split(/\,/, $kk);
        
        my $prichild = shift (@ggg);
        my $pritype  = shift (@ggg);
        my $pritemnum = shift (@ggg);
        my $colpos    = shift (@ggg);

        $prichild =~ s/\%([A-Fa-f0-9]{2})/pack('C', hex($1))/seg;
        $prichild = _stringfix($prichild);
        $pritype  = _stringfix($pritype);

        my $colval = $vv->{colval};

        # fixup colval unless it is a number
        $colval = _stringfix($colval)
            unless ($colval =~ m/^-?(?:\d+(?:\.\d*)?|\.\d+)$/);

        print "INSERT INTO jc_part_rangeitem values($prichild, $pritype, $pritemnum, $colpos, $colval);\n";

    }

} # end  do_insert_part_rangeitem
sub do_insert_partition_rule
{
    my $t_prule = shift; # partition_rule

    my @foo = sort(keys(%{$t_prule}));

    print "\n";

    for my $kk (@foo)
    {
        my $vv = $t_prule->{$kk};

        my ($prchild, $prname, $prparent, $prrangestart, $prrangeend, $prrangeevery);
        

        $prname = _stringfix($vv->{prname}); 
        $prparent = _stringfix($vv->{prparent}); 
        $prrangestart = _stringfix($vv->{prrangestart}); 
        $prrangeend = _stringfix($vv->{prrangeend}); 
        $prrangeevery = _stringfix($vv->{prrangeevery}); 
        $prchild = _stringfix($kk);

        print "INSERT INTO jc_partition_rule values($prchild, $prname, $prparent, $prrangestart, $prrangeend, $prrangeevery);\n";

    }

} # end  do_insert_partition_rule

sub update_catalog
{
    my $reg_tabs = shift;

    # catalog tables

    my $t_part  = {}; # partition table
    my $t_pcols = {}; # part_cols
    my $t_bspec = {}; # part_boundspec
    my $t_ritem = {}; # rangeitem
    my $t_prule = {}; # partition_rule

    my @tabnames = sort(keys(%{$reg_tabs}));

    for my $kk (@tabnames)
    {
        my $vv = $reg_tabs->{$kk};

        next 
            unless (exists($vv->{rules}));

        my $tablename = $kk;
        my $partype;

        if (exists($vv->{rules}))
        {
            my @r1 = sort(keys(%{$vv->{rules}}));

            my $rulecnt = 0;

            for my $kk2 (@r1)
            {
                my $vv2 = $vv->{rules}->{$kk2};

                next
                    unless (exists($vv2->{parsetree})
                            && exists($vv2->{parsetree}->{partSpec}));
                
                $rulecnt++;

                my $childname = $vv2->{child};

                my $partSpec = $vv2->{parsetree}->{partSpec};
                
                $partype = $partSpec->{typ};

                if (exists($t_part->{$tablename}))
                {

                }
                else
                {
                    cat_init_tpart (
                                               $t_part, 
                                               $t_pcols, $tablename, 
                                               $partSpec, $partype);

                }

                my $hrule = {};

                $t_prule->{$vv2->{child}} = $hrule;

                my $bspec_stat = 
                    cat_boundspec (
                                          $t_bspec, 
                                          $t_ritem, $vv2->{child}, 
                                          $partSpec, $partype, $vv2);


                $hrule->{prparent}     = $vv2->{parent};
                $hrule->{prrangestart} = $vv2->{where};

                if (($partype =~ m/range/i) && scalar(@{$bspec_stat}))
                {
                    $hrule->{prrangestart} = shift @{$bspec_stat};
                    $hrule->{prrangeend}   = shift @{$bspec_stat};
                }

            }
        }

    }

    local $Data::Dumper::Sortkeys = 1;

#    print Data::Dumper->Dump([$t_part]);
#    print Data::Dumper->Dump([$t_pcols]);
#    print Data::Dumper->Dump([$t_prule]);
#    print Data::Dumper->Dump([$t_bspec]);
#    print Data::Dumper->Dump([$t_ritem]);

    do_insert_partition_table($t_part);
    do_insert_part_cols($t_pcols);
    do_insert_part_boundspec($t_bspec);
    do_insert_part_rangeitem($t_ritem);
    do_insert_partition_rule($t_prule);



} # end update_catalog

if (0)
{
#    my $txt =" a = b or c = d or e = f";
#    my $txt =" a = b or (c = d) or e = f";
    my $txt =" a = b or ((c = d) or (e = f and g = h)) ";

    my $biga = pw1($txt);

    print Data::Dumper->Dump($biga);

}


if (1)
{
    my $psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

    $psql_str .= " -c \'\\d  \'";

#    print $psql_str, "\n";

    my $tabdef = `$psql_str`;


    if ($tabdef !~ m/List of relations/im)
    {
        exit(0);
    }

    # remove the \d header
    $tabdef =~ s/\s+List of relations\s+//;

#    print $tabdef;

    my $rowarr = tablelizer($tabdef);

    my %reg_tabs;

    for my $rr (@{$rowarr})
    {
        if ($rr->{Type} =~ m/Table/i)
        {
            $reg_tabs{$rr->{Name}} = $rr;
        }

    }

    while (my ($kk, $vv) = each (%reg_tabs))
    {
        $psql_str = "psql ";

        $psql_str .= $glob_connect
            if (defined($glob_connect));

        $psql_str .= " -c \'\\d  $kk\'";
        
        $tabdef = `$psql_str`;

        $vv->{describe} = $tabdef;

        next
            unless ($tabdef =~ m/^Rules\:/m);

#        print $tabdef;


        # look for "Rules: " in column zero, and use it to pattern
        # match remainder of describe output...
        my @ruletxt = ($tabdef =~ m/\nRules\:(.*)/s);

#        print $ruletxt[0], "\n";

        next
            unless (scalar(@ruletxt));

        my $r1 = $ruletxt[0];

        # look for next "<tag>: " in column zero, and use it to
        # delimit set of all rules...
        @ruletxt = split(/\n\w+((\s+\w+)*)\:/s, $r1, 2);
#        @ruletxt = split(/funky/, $r1, 2);

        next
            unless (scalar(@ruletxt));

        $r1 = $ruletxt[0];

        @ruletxt = split(/\n/, $r1);

#        print Data::Dumper->Dump(\@ruletxt);

        my $rules = {};
        my $rulenam;
        my $ruleitm;

        for my $lin (@ruletxt)
        {
            next
                unless (length($lin));

            if ($lin =~ m/AS\s*$/)
            {
                if (defined($rulenam))
                {
                    $rules->{$rulenam} = $ruleitm;
                    $rulenam = undef;
                }

                my @foo = ($lin =~ m/\s+(.*)\s+AS\s*$/);

                unless (scalar(@foo))
                {
                    print STDERR "bad line1 : $lin\n";
                    exit(1);
                }

                $rulenam = $foo[0];
                $ruleitm = {};
            }

            unless (defined($rulenam))
            {
                print STDERR "no rulename\n";
                exit(1);
            }
            
            if ($lin =~ m/^\s+ON INSERT TO\s*.*$/)
            {
                my @foo = ($lin =~ m/^\s+ON INSERT TO\s+(.*)\s*$/);
                
                unless (scalar(@foo))
                {
                    print STDERR "bad line2 : $lin\n";
                    exit(1);
                }
                
                $ruleitm->{parent} = $foo[0];
            }

            if ($lin =~ m/^\s+WHERE.*DO INSTEAD/)
            {
                my @foo = 
       ($lin =~ m/^\s+WHERE\s+(.*)\s+DO INSTEAD\s+INSERT\s+INTO\s+(.*)\s+\(.*$/);

                unless (scalar(@foo) > 0)
                {
                    print STDERR "bad line3 : $lin\n";
                    exit(1);
                }

                $ruleitm->{where} = $foo[0];
                $ruleitm->{child} = $foo[1];
            }


        } # end for ruletxt

        if (defined($rulenam))
        {
            $rules->{$rulenam} = $ruleitm;
            $rulenam = undef;
        }

        $vv->{rules} = $rules;


#        print Data::Dumper->Dump([$vv]), "\n";

    } # end while each reg_tabs



    while (my ($kk, $vv) = each (%reg_tabs))
    {
        if (exists($vv->{rules}))
        {
            my $rr = $vv->{rules};
            while (my ($kk2, $vv2) = each (%{$rr}))
            {
                print STDERR $kk2, ":\n";
                if (exists($vv2->{where}))
                {
                    my ($extracted, $remainder, $txt);

                    $txt = $vv2->{where};
#                    print "  where: ", $txt, "\n";

                    my $biga = pw1($txt);

                    $vv2->{parsetree} = $biga;

                    classify($vv2);
#                    print Data::Dumper->Dump($biga);
                }
            }

        }

    }

    if (0)
    {
    while (my ($kk, $vv) = each (%reg_tabs))
    {
        if (exists($vv->{rules}))
        {
            my $desc = $vv->{describe};
            delete $vv->{describe};
            print Data::Dumper->Dump([$vv]), "\n";

            $vv->{describe} = $desc;
        }

    }}

    print "\n";

    if ($glob_dosql)
    {
        update_catalog(\%reg_tabs);
    }
    else
    {
        prettydump(\%reg_tabs);
    }


}


exit();
