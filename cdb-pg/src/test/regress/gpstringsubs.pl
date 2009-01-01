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

    If the connect string is not defined gpstringsubs uses 
    PGPORT and template1.  If the PGPORT is undefined then 
    the port number defaults to 11000.
        
=back

=head1 DESCRIPTION

gpstringsubs find tokens in an input file and replaces them (in place)
with specified or environmental values, supplementing the pg_regress
"convert_sourcefiles_in" function.

The tokens are:

=over 8

=item hostname

 gpstringsubs finds the hostname for segment 0 
 from the gp_configuration table and replaces all 
 instances of the token @hostname@.

=item ps_pidfirst

 generate a long ps listing with the pid first, equivalent to 
 ps -axww on osx

=item gpwhich_(executable)

 Find the full path for the executable and substitute.  For example, 
 @gpwhich_gpfdist@ is replaced with the full path for "gpfdist".

=item gpuname_(flag)

 Run "uname" with the specified flag and substitute the result.  For
 example, @gpuname_p@ is replaced with the output of "uname -p", which
 finds the generic processor type:

=item gpcurusername

 Replace @gpcurusername@ with the username of the user executing the script.

=over 12

=item Mac OSX intel: i386
=item Mac OSX ppc: powerpc
=item Linux 64: x86_64
=item Linux 32: i686
=item Sun Sparc: sparc
=item Sun Solaris 64: i386

=back

=item gpisainfo

  If on Solaris, run "isainfo" and get the real architecture, eg sparcv9
  or amd64, else just use "uname -p".

=item number_of_segs

  Number of segments.

=item perl_osname

  Use what perl thinks the osname is.  

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

    my $porti = $ENV{PGPORT} if (exists($ENV{PGPORT}));

    $porti = 5432                 # 11000
        unless (defined($porti));

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
    exit(-1)
        unless (scalar(@ARGV));

    my $filnam = $ARGV[0];

    unless (-f $filnam)
    {
        die "invalid filename: $filnam";
        exit(-1);
    }


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

	my $username = getpwuid($>);

    $psql_str = "psql ";
    
    $psql_str .= $glob_connect
        if (defined($glob_connect));

    $psql_str .= " -c \'select count(*) from gp_configuration where content != -1 and isprimary = true \'";

    $tabdef = `$psql_str`;

    $mpp_config_table = tablelizer($tabdef);

    my $numsegs = 0;

#   print Data::Dumper->Dump([$mpp_config_table]);

    for my $rowh (@{$mpp_config_table})
    {
        $numsegs = $rowh->{1};
        last;
    }

    my $numsegexp = '\\@number_of_segs\\@';

    my $hostexp = '\\@hostname\\@';
    my $psexp   = '\\@ps_pidfirst\\@';
	my $unexp = '\\@gpcurusername\\@';

    my $locps   = 'ps axww';
    if ($^O !~ /darwin|mac|osx/i)
    {
        $locps   = 'ps -elf';
    }


    # get the perl osname
    my $perlosexp = '\\@perl_osname\\@';
    my $locperlos = $^O;

    # Sun Solaris only -- use uname -p otherwise
    my $isainfoexp = '\\@gpisainfo\\@';
    
    my $devn = '2>/dev/null';
    my $isainfoloc = `which isainfo $devn`;
    my $locisainfo = `uname -p`;
    chomp $locisainfo;

    if (length($isainfoloc) && ($isainfoloc !~ m/no isainfo/)) # on solaris
    {
        my $realinfo = `isainfo`;
        chomp $realinfo;

        my @foo = split(/\s+/, $realinfo);

        $locisainfo = $foo[0]
            if (scalar(@foo));
    }

    if ($^O !~ /darwin|mac|osx|linux/i)
    {
        # error!
    }

    my $gpwhich_all = `grep gpwhich_ $filnam`;
    my $gpuname_all = `grep uname_ $filnam`;

#    print "$filnam\n";
    system "perl -i -ple \' s/$hostexp/$hostname/gm; s/$psexp/$locps/gm; s/$isainfoexp/$locisainfo/gm; s/$perlosexp/$locperlos/gm; s/$numsegexp/$numsegs/gm; s/$unexp/$username/gm \' $filnam\n";

    # replace all "which" expressions with binary
    if (defined($gpwhich_all) && length($gpwhich_all))
    {
        my @foo = split(/\@/, $gpwhich_all);

#        print Data::Dumper->Dump(\@foo);

        my @gpwhich_list;

        if (scalar(@foo))
        {
            for my $gpw (@foo)
            {
                my @exe_thing;

                next
                    if (($gpw =~ m/is\_transformed\_to/));

                next
                    unless (($gpw =~ m/gpwhich\_\w/));

                @exe_thing = ($gpw =~ m/gpwhich\_(.*)/);

                next
                    unless (scalar(@exe_thing));

                my $binloc = `which $exe_thing[0]`;

                next
                    unless (length($binloc));

                chomp $binloc;
                $binloc = quotemeta($binloc);

                my $whichexp = '\\@gpwhich_' . $exe_thing[0] . '\\@';

                system "perl -i -ple \' s/$whichexp/$binloc/gm;\' $filnam\n";

            } # end for
        } # end if scalar foo
    } # end if which all

    # replace all "gpuname" expressions with "uname -<flag>" output
    if (defined($gpuname_all) && length($gpuname_all))
    {
        my @foo = split(/\@/, $gpuname_all);

#        print Data::Dumper->Dump(\@foo);

        my @gpuname_list;

        if (scalar(@foo))
        {
            for my $gpw (@foo)
            {
                my @flag_thing;

                next
                    if (($gpw =~ m/is\_transformed\_to/));

                next
                    unless (($gpw =~ m/gpuname\_\w/));

                @flag_thing = ($gpw =~ m/gpuname\_(.*)/);

                next
                    unless (scalar(@flag_thing));

                my $flag = $flag_thing[0];
                my $binloc = `uname -$flag`;

                next
                    unless (length($binloc));

                chomp $binloc;
                $binloc = quotemeta($binloc);

                my $unameexp = '\\@gpuname_' . $flag . '\\@';

                system "perl -i -ple \' s/$unameexp/$binloc/gm;\' $filnam\n";

            } # end for
        }
    } # end gpuname


    exit(0);
}
