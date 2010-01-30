#!/usr/bin/perl
#
# $Header$
#
# copyright (c) 2010
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use Getopt::Long;
use Data::Dumper;
use File::Spec;
use strict;
use warnings;

=head1 NAME

B<gpcheckmirrorseg.pl> - check some mirrors

=head1 SYNOPSIS

B<gpcheckmirrorseg> [options] 

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

gpcheckmirrorseg.pl performs validations that the primary and mirror
segments contain the same data.  Sample usage:

perl gpcheckmirrorseg.pl -connect "-p 11000 template1"

Substitute the correct "connect string" for your postgres database.

=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2010 GreenPlum.  All rights reserved.  

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
#            $rowh->{($colhdcnt+1)} = $rawcol;
            $rowh->{$colhd} = $rawcol;
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

	# select from gp_segment_configuration and pg_filespace_entry,
	# constructing a single row for each primary/mirror pair

    $psql_str .= " -c \' ".
		"select gscp.content, " .
		"gscp.hostname as phostname, gscp.address as paddress, " .
		"fep.fselocation as ploc, " .
		"gscm.hostname as mhostname, gscm.address as maddress, " .
		"fem.fselocation as mloc " .
		"from " .
		"gp_segment_configuration gscp, pg_filespace_entry fep, " .
		"gp_segment_configuration gscm, pg_filespace_entry fem " .
		"where " .
		"fep.fsedbid=gscp.dbid and gscp.content > -1 " .
		"and " .
		"fem.fsedbid=gscm.dbid and gscm.content > -1 " .
		"and " .
		# match the filespace ids
		"fem.fsefsoid = fep.fsefsoid " . 
		"and gscp.content = gscm.content " .
		# use "dollar-quoting" to avoid dealing with nested single-quotes:
		# $q$p$q$ is equivalent to 'p'
		"and gscp.role =  " .  '$q$p$q$ ' .
		"and gscm.role =  " .  '$q$m$q$ ' .
		" order by content  \'";

#	print $psql_str, "\n";

    my $tabdef = `$psql_str`;

#   print $tabdef;

	my $gp_seg_config = tablelizer($tabdef);

	$psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

	# checkpoint the database to flush pending writes
    $psql_str .= " -c \' checkpoint \'" ;

	$tabdef = `$psql_str`;

#	print $tabdef;

#	print Data::Dumper->Dump([$gp_seg_config]);

	for my $row (@{$gp_seg_config})
	{
		my $prim_dd = $row->{ploc};
		my $mirr_dd = $row->{mloc};

		my @foo = File::Spec->splitdir($prim_dd);
		my $fil = pop @foo;

		print $fil, "\n";

		my $primfil = $prim_dd;
		my $mirrfil = $mirr_dd;
		
		my $primhost = $row->{phostname};
		my $mirrhost = $row->{mhostname};

		my $primtmp = File::Spec->catfile("/tmp", $fil . ".prim");
		my $mirrtmp = File::Spec->catfile("/tmp", $fil . ".mirr");

		# use openssl to construct an md5 hash for every file
		my $cmd_template = 
			'(gpssh -h MYHOST \' ' .
			'(find MYDIR -type f -exec openssl dgst -md5 -hex {} \; ) ' .
			'| grep -v pg_stat | grep -v pg_log | grep -v pg_internal ' .
			'| grep -v ".log)" | grep -v ".opts)" | grep -v ".conf)" ' .
			'| grep -v "_args)" | grep -v ".pid)" ' .
			'| grep -v "_segargs)" | grep -v "pg_fsm.cache" ' .
			'| grep -v pgstat | grep -v pgsql_tmp  \' ) ' .
			'| sort ' .
			'| perl -i -ple \'s|MYDIR||gm \' ' . 
			# strip out [hostname] prefix
			'| perl -i -ple \'s|^\\s*\\[.*\\]\\s*||gm \' ' . 
			' > ';

		my $cmd = $cmd_template;
		$cmd =~ s/MYHOST/$primhost/g;
		$cmd =~ s/MYDIR/$primfil/g;
		$cmd .= $primtmp;

#		print $cmd, "\n";

		system ($cmd);

		$cmd = $cmd_template;
		$cmd =~ s/MYHOST/$mirrhost/g;
		$cmd =~ s/MYDIR/$mirrfil/g;
		$cmd .= $mirrtmp;

		system ($cmd);

		my $diffi = `diff -b $primtmp $mirrtmp`;

		if (length($diffi))
		{
			print $diffi;

			print Data::Dumper->Dump([$row]);
			die "found differences";
			exit(1)
		}

	}
	
	print "no differences found\n";
	exit(0);

}


exit();
