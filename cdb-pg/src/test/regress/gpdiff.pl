#!/usr/bin/perl
#
# $Header$
#
# Copyright (c) 2007 GreenPlum.  All rights reserved.  
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use strict;
use warnings;
use POSIX;
use File::Spec;

=head1 NAME

B<gpdiff.pl> - GreenPlum diff 

=head1 SYNOPSIS

B<gpdiff.pl> [options] logfile [logfile...]

Options:

Normally, gpdiff takes the standard "diff" options and passes them 
directly to the diff program.  Try `diff --help' for more information
on the standard options.  The following options are specific to gpdiff:

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

gpdiff compares files using diff after processing them with atmsort.pl. 
This comparison is designed to ignore certain Greenplum-specific 
informational messages, as well as handle the cases where query output
order may differ for a multi-segment Greenplum database versus a
single Postgresql instance.  Type "atmsort.pl --man" for more details.
gpdiff is invoked by pg_regress as part of "make install-check".  
In this case the diff options are something like: 

 "-w -I NOTICE: -I HINT: -I CONTEXT: -I GP_IGNORE:".

Like diff, gpdiff can compare two files, a file and directory, a 
directory and file, and two directories.  However, when gpdiff compares
two directories, it only returns the exit status of the diff
comparison of the final two files.  

=head1 BUGS

While the exit status is set correctly for most cases,
STDERR messages from diff are not displayed.


=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2007 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

our $ATMSORT = "atmsort.pl";

# assume atmsort.pl in same directory
BEGIN
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
    my ($f1, $f2, $d2d) = @_;

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

    my $outi =`diff $args`;

    # prefix the diff output with the files names for a "directory to
    # directory" diff
    if (defined($d2d) && length($outi))
    {
        $outi = "diff $f1 $f2\n" . $outi;
    }

    # replace temp file name references with actual file names
    $outi =~ s/$newf1/$f1/gm;
    $outi =~ s/$newf2/$f2/gm;

    print $outi;

#my $stat = WEXITVALUE($?); # diff status
    my $stat = $? >> 8; # diff status

    unlink $newf1;
    unlink $newf2;

    return ($stat);

}

sub filefunc
{
    my ($f1, $f2, $d2d) = @_;

    if ((-f $f1) && (-f $f2))
    {
        return (gpdiff_files($f1, $f2, $d2d));
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

                $stat = filefunc($absname, $f2, 1);
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

        $stat = filefunc($f1, $fnam, $d2d);

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

        $stat = filefunc($fnam, $f2, $d2d);

        return $stat;
    }

    return 0;
}

if (1)
{
    my $man  = 0;
    my $help = 0;
    my $pmsg = "";

#    getatm();

    # check for man or help args
    if (scalar(@ARGV))
    {
        if ($ARGV[0] =~ m/^\-(\-)*(man|help|\?)$/i)
        {
            if ($ARGV[0] =~ m/man/i)
            {
                $man = 1;
            }
            else
            {
                $help = 1;
            }
        }
    }
    else
    {
        $pmsg = "missing an operand after \`gpdiff\'";
        $help = 1;
    }

    if ((1 == scalar(@ARGV)) && (!($help || $man)))
    {
        $pmsg = "unknown operand: $ARGV[0]";
        $help = 1;
    }

    pod2usage(-msg => $pmsg, -exitstatus => 1) if $help;
    pod2usage(-msg => $pmsg, -exitstatus => 0, -verbose => 2) if $man;

    my $f2 = pop @ARGV;
    my $f1 = pop @ARGV;

    for my $fname ($f1, $f2)
    {
        unless (-e $fname)
        {
            print STDERR "gpdiff: $fname: No such file or directory\n";
        }
    }
    exit(2)
        unless ((-e $f1) && (-e $f2));

    exit(filefunc($f1, $f2));
}
