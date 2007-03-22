#!/usr/bin/perl
#
# $Header$
#
# Copyright (c) 2007 GreenPlum.  All rights reserved.  
# Author: Jeffrey I Cohen
#
#
use strict;
use warnings;
use POSIX;
use File::Spec;

our $ATMSORT = "atmsort.pl";

# assume atmsort.pl in same directory
sub getatm
{
    my $plname = $0;

    my @foo = File::Spec->rel2abs(File::Spec->splitpath($plname));

    return 0
        unless (scalar(@foo));

    pop @foo;

    $ATMSORT =
        File::Spec->rel2abs(
                            File::Spec->catfile(
                                                @foo,
                                                "atmsort.pl"
                                                ));

}

sub gpdiff_files
{
    my ($f1, $f2) = @_;

    my @tmpfils;

    for my $ii (1..2)
    {
        my $tmpnam;

        for (;;) 
        {
            my $tmpfh;
        
            $tmpnam = tmpnam();
            sysopen($tmpfh, $tmpnam, O_RDWR | O_CREAT | O_EXCL) && last;
        }

        push @tmpfils, $tmpnam;
        
    }
        
    my $newf1 = shift @tmpfils;
    my $newf2 = shift @tmpfils;

#    print $ATMSORT, "\n";

    system "$ATMSORT < $f1 > $newf1";
    system "$ATMSORT < $f2 > $newf2";

    my $args = join(" ", @ARGV, $newf1, $newf2);

    system "diff $args";

#my $stat = WEXITVALUE($?); # diff status
    my $stat = $? >> 8; # diff status

    unlink $newf1;
    unlink $newf2;

    return ($stat);

}

sub filefunc
{
    my ($f1, $f2) = @_;

    if ((-f $f1) && (-f $f2))
    {
        return (gpdiff_files($f1, $f2));
    }

    # if f1 is a directory, do the filefunc of every file in that directory
    if ((-d $f1) && (-d $f2))
    {
        my $dir = $f1;
        my ($dir_h, $stat);

        if ( opendir($dir_h, $dir) ) 
        {
            my $fnam;
            while ($fnam = readdir($dir_h))
            {
                # ignore ., ..
                next
                    unless ($fnam !~ m/^(\.)(\.)*$/);

                my $absname =
                    File::Spec->rel2abs(
                                        File::Spec->catfile(
                                                            $dir,
                                                            $fnam
                                                            ));

                $stat = filefunc($absname, $f2);
            } # end while
            closedir $dir_h;
        } # end if open
        return $stat;
    }

    # if f2 is a directory, find the corresponding file in that directory
    if ((-f $f1) && (-d $f2))
    {
        my $stat;
        my @foo = File::Spec->splitpath($f1);

        return 0
            unless (scalar(@foo));
        my $basenam = $foo[-1];

        my $fnam = 
            File::Spec->rel2abs(
                                File::Spec->catfile(
                                                    $f2,
                                                    $basenam
                                                    ));

        $stat = filefunc($f1, $fnam);

        return $stat;
    }

    # find f2 in dir f1
    if ((-f $f2) && (-d $f1))
    {
        my $stat;
        my @foo = File::Spec->splitpath($f2);

        return 0
            unless (scalar(@foo));
        my $basenam = $foo[-1];

        my $fnam = 
            File::Spec->rel2abs(
                                File::Spec->catfile(
                                                    $f1,
                                                    $basenam
                                                    ));

        $stat = filefunc($fnam, $f2);

        return $stat;
    }

    return 0;
}

if (1)
{
    getatm();

    my $f2 = pop @ARGV;
    my $f1 = pop @ARGV;

    exit(filefunc($f1, $f2));
}
