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

B<gpsourcify.pl> - GreenPlum reverse string substitution 

=head1 SYNOPSIS

B<gpsourcify.pl> filename

Options:

    -help            brief help message
    -man             full documentation
    -token_file      file with list of tokens and replacement values 
                     (gptokencheck by default)
    -nodlsuffix      do not replace DLSUFFIX (true by default)

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-token_file> (gptokencheck by default)

    File which contains a list of token/value pairs.   
        
=back

=head1 DESCRIPTION

gpsourcify reads a pg_regress ".out" file as input and rewrites it to
stdout, replacing certain local or environmental values with portable
tokens.  The output is a candidate for a pg_regress ".source" file.
gpsourcify is the complement to the gpstringsubs and pg_regress
"convert_sourcefiles_in" functions.  


=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2007 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

    my $glob_id = "";

my $glob_token_file;

BEGIN {
    my $man  = 0;
    my $help = 0;

    GetOptions(
               'help|?' => \$help, man => \$man
               )
        or pod2usage(2);


    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    $glob_token_file = "results/gptokencheck.out";

#    print "loading...\n";                                                     
}


sub LoadTokens
{
    my $fil = shift;

    return undef
        unless (defined($fil) && (-f $fil));

    my $ini = `grep 'is_transformed_to' $fil`;

    $ini =~ s/^\-\-\s+//gm;

#    print $ini;

    my @foo = split(/\n/, $ini);

    my $bigh = {};

    for my $kv (@foo)
    {
        my @baz = split(/\s*\#is\_transformed\_to\#\s*/, $kv, 2);
        
        if (2 == scalar(@baz))
        {
            $bigh->{$baz[0]} = quotemeta($baz[1]);
        }

    }

#    print Data::Dumper->Dump([$bigh]);

    return $bigh;
}


if (1)
{
    my $bigh = LoadTokens($glob_token_file);
    exit(0) unless (defined($bigh));

    # make an array of the token names (keys), sorted descending by
    # the length of of the replacement value.
    my @sortlen;
    @sortlen = 
        sort {length($bigh->{$b}) <=> length($bigh->{$a})} keys %{$bigh};

#    print Data::Dumper->Dump(\@sortlen);

    while (<>)
    {
        my $ini = $_;

        for my $kk (@sortlen)
        {
            my $vv = $bigh->{$kk};
            my $k2 = '@' . $kk . '@';

            if ($kk =~ m/DLSUFFIX/)
            {
                # ignore for now...
                next;
            }

            if ($kk =~ m/abs_srcdir/)
            {
                my $v2 = $vv . "/data";
                my $k3 = $k2 . "/data";
                # replace the srcdir only if followed by "/data"
                $ini =~ s/$v2/$k3/g;                
            }
            elsif ($kk =~ m/abs_builddir/)
            {
                # replace buildir if not followed by "/data"
                $ini =~ s/$vv(?!\/data)/$k2/g;
            }
            else
            {
                $ini =~ s/$vv/$k2/g;
            }

        }

        print $ini;

    }
   

    exit(0);
}
