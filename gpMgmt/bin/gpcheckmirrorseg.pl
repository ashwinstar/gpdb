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
    -ignore          filename pattern to ignore during comparison

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-connect>
    
    psql connect string, e.g:

    -connect '-p 11000 template1'

=item B<-ignore>

Specify a filename pattern that should be exclude when comparing data
directories.  To specify multiple patterns, use multiple ignore
arguments, eg:

  -ignore gp_dump -ignore pg_dump


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

my $glob_port;

my $glob_ignore;

my $glob_filedump;

BEGIN {
    my $man  = 0;
    my $help = 0;
    my $conn;
	my @ignore;

    GetOptions(
               'help|?' => \$help, man => \$man, 
               "connect=s" => \$conn,
		       'exclude|ignore:s' => \@ignore
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    $glob_connect = $conn;
    $glob_ignore = \@ignore;

	my $gp_filedump = `which gp_filedump`;
	chomp $gp_filedump;

	$glob_filedump = $gp_filedump if (defined($gp_filedump) && 
									  length($gp_filedump))

#    print "loading...\n" ;
}

sub perlhash
{
	# version of the original perl hash function
	my $ini = shift;

	my $hash = 0;

	for my $val (split (//, $ini))
	{
		$hash = $hash*33 + ord($val);
	}
	return $hash;
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

# build a list of expressions to ignore
sub ignore_list
{
	my $iggy = shift;

	my @m1 = qw(
		pg_stat 
		pg_log
		pg_internal
		pg_fsm.cache
		pgstat
		pgsql_tmp
		);

	# file suffixes -- append a close paren ")"
	my @m2 = qw(
		.log
		.opts
		.conf
		_args
		.pid
		core
		_segargs
		);

	for my $ww (@m2)
	{
		push @m1, $ww . ")";
	}

	# add any other expressions (eg user-supplied)
	push @m1, @{$iggy} if (defined($iggy) && scalar(@{$iggy}));

	my @master;

	# build of list of grep exclusion commands
	for my $ww (@m1)
	{
		push @master, 'grep -v "' . $ww . '"';
	}

	return \@master;
	
}

sub get_presh_txt
{
	my $bigstr = <<'EOF_bigstr';
##tiny perl script to list filename, filesize, md5 hash
#for my $filnam (@ARGV) # xargs supplies up to 5000 args
#{
#  my ($dev,$ino,$mode,$nlink,$uid,$gid,$rdev,$size,
#      $atime,$mtime,$ctime,$blksize,$blocks) = stat($filnam);
# my $sz = $size;
# print $filnam, " ", $sz, " ";
# print `openssl dgst -md5 -hex $filnam`
#}
EOF_bigstr

	$bigstr =~ s/^\#//gm;

	return $bigstr;
}

sub get_presh2_txt
{
	my $bigstr = <<'EOF_bigstr';
##nontiny perl script to preprocess ao or heap files
##by truncating to length (ao) or filtering hint bits (heap)
##
## args: -adler|noadler -heap|(-ao <filesize> ) filename
##
## if adler is specified, prints blockno and adler32 for each block,
## else if noadler, just preprocesses each block according to ao/heap rules
## and prints it out.
#
#my $glob_adler = 0;
#
#sub doprint
#{
#    my ($blockno, $buf) = @_;
#	
#    if ($glob_adler)
#    {
#		print $blockno, ":", adler32(1, [$buf]), "\n";
#    }
#    else
#    {
#		print $buf;
#    }
#    
#}
#
## adapted from Digest::Adler32
## see RFC 1950 section 9
#sub adler32 
#{
#    my ($ad32, $bufs) = @_;
#	
#    my $s1 = $ad32 & 0x0000FFFF;
#    my $s2 = ($ad32 >> 16) & 0x0000FFFF;
#
#    for my $buf (@{$bufs}) 
#    {
#		for my $c (unpack("C*", $buf)) 
#		{
#			$s1 = ($s1 + $c ) % 65521;
#			$s2 = ($s2 + $s1) % 65521;
#		}
#    }
#    $ad32 = ($s2 << 16) + $s1;
#    return $ad32;
#}
#
#if (1)
#{
#	my $flag1 = shift @ARGV;
#	my $flag2 = shift @ARGV;
#	my ($sz, $orig_sz, $cur_siz);
#	my $blockno = 0;
#	
#	if ($flag1 =~ m/noadler/i)
#	{
#		$glob_adler = 0;
#	}
#	else
#	{
#		$glob_adler = 1;
#	}
#	
#	if ($flag2 =~ m/ao/i)
#	{
#		$sz = shift @ARGV;
#		$orig_sz = $sz;
#		$cur_sz = 0;
#	}
#	my $filnam = shift @ARGV;
#	
#	if (defined($sz))
#	{
#		my $fh;
#		
#		open ($fh, "<", $filnam) or die ("cannot open $filnam: $!");
#		my $bufsiz = 32768;
#		my $bufl;
#		
#		if ($bufsiz > $sz)
#		{
#			$bufl = $sz;
#			$sz = 0;
#		}
#		else
#		{
#			$sz -= $bufsiz;
#			$bufl = $bufsiz;
#		}
#		
#		while ($bufl)
#		{
#			my $buf;
#			my $readlen = read($fh, $buf, $bufl);
#	        doprint($blockno, $buf);
#			
#			$cur_sz += $readlen;
#			
#			if ($readlen && ($readlen < $bufl))
#			{
#				warn "file $filnam shorter than $orig_sz bytes";
#			    die "read only $cur_sz bytes (last block $readlen bytes vs $bufl)" ;
#			}
#			
#			if ($bufsiz > $sz)
#			{
#				$bufl = $sz;
#				$sz = 0;
#			}
#			else
#			{
#				$sz -= $bufsiz;
#				$bufl = $bufsiz;
#			}
#			$blockno++;
#		}
# 	    close $fh;
#	} # else heap
#	else
#	{
#		my $fh;
#		
#		open ($fh, "<", $filnam) or die ("cannot open $filnam: $!");
#		
#		my $bufsiz = 32768;
#		my $buf;
#		my $realsiz;
#		
#		while (1)
#		{
#			$realsiz = read($fh, $buf, $bufsiz);
#			
#			if ($realsiz < $bufsiz)
#			{
#	            doprint($blockno, $buf);
#				last;
#			}
#			
#			# bits            64 16         
#			# bytes            8 2
#			my @foo = unpack("L2 S S S S S S ", $buf);
#			
#			shift @foo; # discard XLogRecPtr
#			shift @foo;
#			
#			shift @foo; # discard pd_tli, pd_flags
#			shift @foo;
#			
#			my $pd_lower = shift @foo;
#			my $pd_upper = shift @foo;
#			my $pd_special = shift @foo;
#			
#			my $pd_pagesize_version = shift @foo;
#			
#			# print "page size: ", $pd_pagesize_version & 0xFF00, "\n";
#			# print "page version: ", $pd_pagesize_version & 0x00FF, "\n";
#			
#			my $szofpagehdrdata = 8 + (6 * 2);
#			
#			my $maxoffsetnum = ($pd_lower <= $szofpagehdrdata) ? 0 : 
#				# divide by sizeof ItemIdData
#				(($pd_lower - $szofpagehdrdata) / 4 ); 
#			
#			my $twomax = 2 * $maxoffsetnum ;
#			
#			my @baz = unpack("L2 S S S S S S S$twomax", $buf);			
#			
#			for my $jj (1..8)
#			{
#				shift @baz
#					;
#			}
#			
#			my $ii = 0;
#			
#			while ($ii < scalar(@baz))
#			{
#				my $s1 = $baz[$ii];
#				my $s2 = $baz[$ii+1];
#				
#				$ii += 2;
#				
#				my $lp_off = $s1;
#				$lp_off = $lp_off & 0x7FFF;
#				
#				my $lp_flags = 0;
#				
#				my $lp_len = $s2 >> 1;
#				
#				my @ggg = unpack("x$lp_off S11 c c  ", $buf);
#				
#				# clear infomask flags
#				$ggg[10] = 0;
#				
#				my $fixbuf = pack("S11 c c  ", @ggg);
#				substr($buf, $lp_off, length($fixbuf)) = $fixbuf;
#				
#			} # end while
#			
#	        doprint($blockno, $buf);
#			$blockno++;
#		}
#		
#		close $fh;
#	}
#}
EOF_bigstr

	$bigstr =~ s/^\#//gm;

	if (defined($glob_filedump))
	{
		$bigstr =~ s/MYFILEDUMP/$glob_filedump/gm;
	}
	else
	{
		$bigstr =~ s/MYFILEDUMP//gm; # empty string
	}

	return $bigstr;
}

# diff primary and mirror file lists. entries are of the form:
#  filename filesize md5hash
#
# build a hash listing "extra" files (no match on primary/mirror),
# plus diffs in file sizes and hashes
sub dandydiff
{
	my ($prim, $mirr) = @_;

	my ($pfh, $mfh);

	open ($pfh, '<', $prim) or die ("cannot open $prim: $!");
	open ($mfh, '<', $mirr) or die ("cannot open $mirr: $!");
		
	my ($plin, $mlin);

	my $pstat = ($plin = <$pfh>);
	my $mstat = ($mlin = <$mfh>);

	my (@extrafiles, %sizediff, %hashdiff);

	while ($pstat && $mstat)
	{
		$plin =~ s/\s+/ /gm;
		$mlin =~ s/\s+/ /gm;

		goto L_dandy1 if ($plin eq $mlin);

		# check filenames
		my @ppp = split(/\s+/, $plin);
		my @mmm = split(/\s+/, $mlin);

		die ("bad line: $plin\nin file: $prim") unless (scalar(@ppp));
		die ("bad line: $mlin\nin file: $mirr") unless (scalar(@mmm));
		die ("bad something: $plin\n$mlin")
			unless (scalar(@ppp) == scalar(@mmm));

		my $pfilnam = shift @ppp;
		my $mfilnam = shift @mmm;

		# if filenames don't match, advance the lesser (and mark it as
		# an "extra")
		if ($pfilnam gt $mfilnam)
		{
			push @extrafiles, "m:". $mfilnam;
			$mstat = ($mlin = <$mfh>);
			next;
		}
		elsif ($pfilnam lt $mfilnam)
		{
			push @extrafiles, "p:". $pfilnam;
			$pstat = ($plin = <$pfh>);
			next;
		}

		# filenames match, so check file sizes
		my $pfilsiz = shift @ppp;
		my $mfilsiz = shift @mmm;
		
		if ($pfilsiz != $mfilsiz)
		{
			$sizediff{$pfilnam} = [$pfilsiz, $mfilsiz];

			# this is ok for ao tables -- need to check eof in db.
			# don't bother checking the hash -- they won't match, either

			goto L_dandy1;
		}

		# file sizes match, so check hash
		my $pfilhsh = pop @ppp;
		my $mfilhsh = pop @mmm;

		if ($pfilhsh eq $mfilhsh)
		{
			# uh oh -- why did we diff if filenames match, file sizes
			# match, and md5 hashes match??

			die ("bad hash funkiness: $plin\n$mlin");
		}

		# either a case of hint bits for heap tables, or aborted txns for ao
		$hashdiff{$pfilnam} = [$pfilhsh, $mfilhsh];		

	  L_dandy1:
		$pstat = ($plin = <$pfh>);
		$mstat = ($mlin = <$mfh>);
	}

	# pstat and mstat should both hit EOF at same time, otherwise have
	# extra files

	while ($pstat)
	{
		# check filenames
		my @ppp = split(/\s+/, $plin);

		warn ("bad line: $plin\nin file: $prim") unless (scalar(@ppp));

		my $pfilnam = shift @ppp;

		push @extrafiles, "p:". $pfilnam;

		$pstat = ($plin = <$pfh>);
	}

	while ($mstat)
	{
		# check filenames
		my @mmm = split(/\s+/, $mlin);

		warn ("bad line: $mlin\nin file: $mirr") unless (scalar(@mmm));

		my $mfilnam = shift @mmm;

		push @extrafiles, "m:". $mfilnam;

		$mstat = ($mlin = <$mfh>);
	}

	close $pfh;
	close $mfh;

	my %bigh = (extrafiles => \@extrafiles, sizediff => \%sizediff, 
				hashdiff => \%hashdiff);

	return \%bigh;

} # end dandydiff

sub get_relation_info_query_txt
{
	my $bigstr = <<'EOF_bigstr';
select
pt.spcname,
prn.tablespace_oid,
pd.datname,
prn.database_oid,
pc.relname,
pc.oid as relid,
prn.relfilenode_oid,
pc.relfilenode,
pc.relisshared,
pc.relstorage,
pc.relkind
from gp_persistent_relation_node as prn,
pg_class as pc,
pg_tablespace as pt,
pg_database as pd
where prn.tablespace_oid = pt.oid
and prn.database_oid = pd.oid
and prn.relfilenode_oid = pc.relfilenode
and pc.relfilenode in (
EOF_bigstr

	return $bigstr;
} # end get_relation_info_query_txt


sub get_aoseg_func
{
	my ($rkey, $rnod, $dbname, $diffi, $row, $fselochsh, $bigh) = @_;

	my $psql_str = "psql ";

	# XXX XXX: need portnumber!!
	$psql_str .= "-p $glob_port " . $dbname;

	# select from gp_persistent_relation_node, pg_class, etc

	# use "dollar-quoting" to avoid dealing with nested single-quotes:
	# $q$p$q$ is equivalent to 'p'
	$psql_str .= " -c \' " .
		"select gp_segment_id,* from gp_dist_random(" . '$q$' .
		"pg_aoseg.pg_aoseg_" . $rkey . '$q$' . ") " .
		# filter by content/segment_id (which is identical for primary
		# and mirror, ie not the dbid)
		"where gp_segment_id = " . $row->{content} .
		"\' ";

#	print $psql_str, "\n";

	my $tabdef = `$psql_str`;

#	print $tabdef;

	my $ao_seg = tablelizer($tabdef);

	return $ao_seg;
}


# helper function for resolve_sizediff, resolve_hashdiff
sub get_relfilenode_func
{
	my ($h1, $diffi, $row, $fselochsh, $bigh) = @_;

	my @relfnods;
	my %dbs;

	# list of filenames for hash or size diff
	for my $spec (keys(%{$h1}))
	{
		# spec like: /base/10892/139321.2'

		my @foo = split(/\//, $spec);

		die "bad spec: $spec" unless scalar(@foo);

		my $fil			= pop @foo;
		my $db			= pop @foo; # normally a dbid, but could be "global";
		my $relfilenode = $fil;

		$relfilenode =~ s/\.\d+$//gm; # remove the trailing ".number"...

		# must be a number for dbid -- will error out later
		warn "invalid dbid: $db" unless ($db =~ m/^((\d+)|global)$/);

		$dbs{$db} = {relfilenodeh=>{}} unless exists($dbs{$db});

		unless (exists($dbs{$db}->{relfilenodeh}->{$relfilenode}))
		{
			$dbs{$db}->{relfilenodeh}->{$relfilenode} = { files => [] } ;

			# Build a list of relfilenodes to lookup in catalog.
			# Note: filter out non-numeric names like "pg_auth"
			if ($relfilenode =~ m/^\d+$/)
			{
				push @relfnods, $relfilenode;
			}
			else
			{
				warn "invalid relfilenode: $relfilenode in db: $db";
			}

		}
		
		push @{$dbs{$db}->{relfilenodeh}->{$relfilenode}->{files}}, $fil;
	}

    my $psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

	# only read from "connectable" db's
    $psql_str .= " -c \' " . 
		"select oid, datname from pg_database where datallowconn  \' ";

#	print $psql_str, "\n";

    my $tabdef = `$psql_str`;

	print "databases that allow connect:\n", $tabdef;

	my $dbidinfo = tablelizer($tabdef);

	# Note: treat all global objects as belonging to template1 (dbid=1)
	if (exists($dbs{global}))
	{
		$dbs{global}->{dbname} = "template1";
	}

	# find the dbnames for the dbids
	for my $rr (@{$dbidinfo})
	{
		if (exists($dbs{$rr->{oid}}))
		{
			$dbs{$rr->{oid}}->{dbname} = $rr->{datname};
		}
	}

#	print Data::Dumper->Dump([%dbs]);

	for my $dbkey (keys(%dbs))
	{
		next
			unless (exists($dbs{$dbkey}->{dbname}));

		$psql_str = "psql ";

		# XXX XXX: need portnumber!!
		$psql_str .= "-p $glob_port " . $dbs{$dbkey}->{dbname};

		# select from gp_persistent_relation_node, pg_class, etc
		$psql_str .= " -c \' ". get_relation_info_query_txt()
			. join (", ", @relfnods) . ") \'";

#		print $psql_str, "\n";

		$tabdef = `$psql_str`;

		print $tabdef;

		my $gp_prn = tablelizer($tabdef);

		for my $rr2 (@{$gp_prn})
		{
			if (exists($dbs{$dbkey}->{relfilenodeh}->{$rr2->{relfilenode}}))
			{
				my $rnod = $dbs{$dbkey}->{relfilenodeh}->{$rr2->{relfilenode}};

				$rnod->{relname}	 = $rr2->{relname};
				$rnod->{relid}		 = $rr2->{relid};
				$rnod->{relisshared} = $rr2->{relisshared};
				$rnod->{relstorage}	 = $rr2->{relstorage};
				$rnod->{relkind}	 = $rr2->{relkind};
			}
		}

	}

	# sanity check
	my $bstat = 1;

	# for all db's
	while (my ($kk, $vv) = each(%dbs))	
	{
		my $dbname;
		if (exists($vv->{dbname}))
		{
			$dbname = $vv->{dbname};
		}
		else
		{
			$bstat = 0;
			warn "no dbname for $kk";
			$dbname = "dbid: $kk";
		}

		# for all relfilenodes
		while (my ($kk2, $vv2) = each(%{$vv->{relfilenodeh}}))
		{
			my $rnod = $vv2;
			my $relname;

			if (exists($rnod->{relname}))
			{
				$relname = $rnod->{relname};
			}
			else
			{
				$bstat = 0;
				print Data::Dumper->Dump([$vv2]),"\n";
				warn "bad relfilenode $kk2 for $dbname -- no relname";
				$relname = "relfilenode: $kk2";
				next;
			}

			unless (exists($rnod->{relkind})
					&& ($rnod->{relkind} =~ m/^(r|o|t)$/))
			{
				$bstat = 0;
				print Data::Dumper->Dump([$vv2]),"\n";
				warn "$relname for $dbname not of kind relation (r), toast (t) or aoseg (o)";
				next;
			}

			unless (exists($rnod->{relstorage})
					&& ($rnod->{relstorage} =~ m/^(a|h)$/))
			{
				$bstat = 0;
				print Data::Dumper->Dump([$vv2]),"\n";
				warn "$relname for $dbname storage is not ao or heap";
			}
			# XXX XXX: what about for column-oriented?? 
			if (exists($rnod->{relstorage})
					&& ($rnod->{relstorage} eq "a"))
			{
				$rnod->{aoseg} = 
					get_aoseg_func($kk2, $rnod, $dbname,
								   $diffi, $row, $fselochsh, $bigh);

				$rnod->{aoseg_eof} = {};

				# build hash by segno (ie file.segno) of eof lengths
				for my $rr2 (@{$rnod->{aoseg}})
				{
					$rnod->{aoseg_eof}->{$rr2->{segno}} = $rr2->{eof};
				}
			}

		} # end for all relfilenodes

	} # end for all db's

	my $do_anyway = 1;

	unless ($do_anyway || $bstat)
	{
		print Data::Dumper->Dump([%dbs]);

		print $diffi;

		print Data::Dumper->Dump([$row]);

		print Data::Dumper->Dump([$bigh]);	 

		die "too many failures resolving relfilenodes";
		exit(1);
	}

	return \%dbs;
} # end get_relfilenode_func

sub resolve_sizediff
{
	my ($diffi, $row, $fselochsh, $bigh) = @_;
	
	my $dbs = 
		get_relfilenode_func($bigh->{sizediff},
							 $diffi, $row, $fselochsh, $bigh);

	my $bstat = 1;

	# for all db's
	while (my ($kk, $vv) = each(%{$dbs}))	
	{
		my $dbname = $vv->{dbname};

		# for all relfilenodes
		while (my ($kk2, $vv2) = each(%{$vv->{relfilenodeh}}))
		{
			my $rnod = $vv2;
			my $relname = exists($rnod->{relname}) ? $rnod->{relname} : 
				"<unknown relname: $kk2>";

			unless (exists($rnod->{relstorage})
					&& ($rnod->{relstorage} =~ m/^(a)$/))
			{
				$bstat = 0;
				print Data::Dumper->Dump([$vv2]),"\n";
				warn "bad sizediff for non-ao file $relname for db $dbname";
			}
		} # end for all relfilenodes

	} # end for all db's

	my $do_anyway = 1;

	unless ($do_anyway || $bstat)
	{
		print Data::Dumper->Dump([$dbs]);

		print $diffi;

		print Data::Dumper->Dump([$row]);

		print Data::Dumper->Dump([$bigh]);	 

		die "found size differences in non-ao files";
		exit(1);
	}

	return $dbs;

} # end resolve_sizediff

sub resolve_hashdiff
{
	my ($diffi, $row, $fselochsh, $bigh) = @_;

	my $dbs = 
		get_relfilenode_func($bigh->{hashdiff},
							 $diffi, $row, $fselochsh, $bigh);


	return $dbs;

} # end resolve_hashdiff

sub diff_again
{
	my ($dbs1, $dbs2,
		$diffi, $row, $fselochsh, $bigh,
		$primfil, $mirrfil, $primhost, $mirrhost, 
		$primtmp, $mirrtmp,
		$ppresh2, $mpresh2,
		$primsh, $mirrsh) = @_;

	my $bstat = 1;
	my (@aofiles, @heapfiles, @unknownfiles);

	for my $dbs ($dbs1, $dbs2)
	{
		while (my ($kk, $vv) = each(%{$dbs}))	
		{
			my $dbname = $vv->{dbname};

			# for all relfilenodes
			while (my ($kk2, $vv2) = each(%{$vv->{relfilenodeh}}))
			{
				my $rnod = $vv2;
				my $relname = exists($rnod->{relname}) ? $rnod->{relname} : 
					"<unknown relname: $kk2>";

				{
					my @filpref;
					
					push @filpref, "base" unless ($kk eq "global");
					push @filpref, $kk; # dbid

					# build full path to filename
					for my $f1 (@{$rnod->{files}})
					{
						my $filnam = join("/", @filpref) . "/" . $f1;

						if (exists($rnod->{relstorage})
							&& ($rnod->{relstorage} =~ m/h|a/))
						{
							if ($rnod->{relstorage} eq "h")
							{
								push @heapfiles, $filnam;
							}
							elsif ($rnod->{relstorage} eq "a")
							{
								my @foo = split(/\./, $filnam);
								
								die "bad fil no segno" unless scalar(@foo);

								my $segno = pop @foo;
								my $eof;

								die "no eof for segno $segno" 
								unless (exists($rnod->{aoseg_eof}->{$segno}));

								$eof = $rnod->{aoseg_eof}->{$segno};
								
								push @aofiles, {file => $filnam, eof=> $eof};
							}
						}
						else 
						{
							my $s1 = exists($rnod->{relstorage}) ? 
								$rnod->{relstorage} : "<unknown>";
							warn "unknown storage type: $s1 for $relname in $dbname";
							push @unknownfiles, $filnam;
						}

					}

				}

			} # end for all relfilenodes
			
		} # end for all db's
	}

	my (@allfiles1, @allfiles2);

	for my $fil (@aofiles)
	{
		my $eof = $fil->{eof};

		# calculate hash for files, but prefix hash with filename
		push @allfiles1, 
		$fil->{file} ."  ( perl MYPRESH21 -noadler -ao  $eof  MYDIR/" . $fil->{file}
		. " | openssl dgst -md5 -hex ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil->{file} . 
		':ao: ", $_ ;}' . "'";
	}
	for my $fil (@heapfiles)
	{
		push @allfiles1, 
		$fil . "  ( perl MYPRESH21 -noadler -heap  MYDIR/" . $fil
		. " | openssl dgst -md5 -hex ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil . 
		':heap: ", $_ ;}' . "'";
###			File::Spec->catfile($primfil, $fil->{file});
	}
	for my $fil (@unknownfiles)
	{
		push @allfiles1, 
		$fil . "  ( cat MYDIR/" . $fil
		. " | openssl dgst -md5 -hex ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil . 
		':unknown: ", $_ ;}' . "'";
###			File::Spec->catfile($primfil, $fil->{file});
	}

#	print Data::Dumper->Dump([$dbs1]);
#	print Data::Dumper->Dump([$dbs2]);

	# need filename to sort correctly
	@allfiles2 = sort @allfiles1;
	@allfiles1 = ();

	# remove the filename sort key, leaving command string
	for my $fff (@allfiles2)
	{
		my @zzz = split(/\s/, $fff, 2);
		shift @zzz;
		push @allfiles1, $zzz[0];
	}


	my $numfiles = scalar(@allfiles1);
	my $floc = $row->{ploc};

	print "recheck $numfiles files for $floc\n";
	print Data::Dumper->Dump(\@allfiles1);

	$primsh .= "_deux";
	$mirrsh .= "_deux";
	$primtmp .= "_deux";
	$mirrtmp .= "_deux";

	# use openssl to construct an md5 hash for every file
	my $cmd_template = join ("\n", @allfiles1);
	my $cmd = $cmd_template;
	$cmd =~ s/MYHOST/$primhost/g;
	$cmd =~ s/MYDIR/$primfil/g;
	$cmd =~ s/MYOUT/$primtmp/g;
	$cmd =~ s/MYPRESH2/$ppresh2/g;

	my $fh;
	open ($fh, '>', $primsh);

	print $fh $cmd, "\n";

	close $fh;

	$cmd = $cmd_template;
	$cmd =~ s/MYHOST/$mirrhost/g;
	$cmd =~ s/MYDIR/$mirrfil/g;
	$cmd =~ s/MYOUT/$mirrtmp/g;
	$cmd =~ s/MYPRESH2/$mpresh2/g;

	open ($fh, '>', $mirrsh);

	print $fh $cmd, "\n";

	close $fh;

	# copy the shell scripts to the segments, execute them, then
	# copy back the results. Note that we add "1" as a suffix to
	# the script name and output file name to handle the case
	# where the master and segment are on the same machine (useful
	# for testing)

	my $cmd_template2 = 
			'gpscp -h MYHOST MYSHSCRIPT =:MYSHSCRIPT1 ; ' .
			'gpssh -h MYHOST \' sh MYSHSCRIPT1 > MYOUT  \' ; ' .
			'gpscp -h MYHOST  =:MYOUT MYOUT1 ; ' ;

	$cmd = $cmd_template2;
	$cmd =~ s/MYHOST/$primhost/g;
	$cmd =~ s/MYDIR/$primfil/g;
	$cmd =~ s/MYOUT/$primtmp/g;
	$cmd =~ s/MYSHSCRIPT/$primsh/g;

	print $cmd, "\n";

	system ($cmd);

	$cmd = $cmd_template2;
	$cmd =~ s/MYHOST/$mirrhost/g;
	$cmd =~ s/MYDIR/$mirrfil/g;
	$cmd =~ s/MYOUT/$mirrtmp/g;
	$cmd =~ s/MYSHSCRIPT/$mirrsh/g;

	print $cmd, "\n";

	system ($cmd);

	# add "1" suffix on master
	$primtmp .= '1';
	$mirrtmp .= '1';

	my $diffi2 = `diff -b $primtmp $mirrtmp`;

	if (length($diffi2))
	{
		print "enhanced diff: found differences\n";

		my $fdiff = $diffi2;

		$fdiff =~ s/\:ao\:\s+.*/:ao:/gm;
		$fdiff =~ s/\:heap\:\s+.*/:heap:/gm;
		$fdiff =~ s/\:unknown\:\s+.*/:unknown:/gm;
		$fdiff =~ s/^\>\s+//gm;
		$fdiff =~ s/^\<\s+//gm;

		my %thrice;
		my @foo = split(/\n/, $fdiff);

		for my $ff (@foo)
		{
			next
				unless ($ff =~ m/\:ao\:|\:heap\:|\:unknown\:/);
			$thrice{$ff} = 1;
		}
		print "block by block: ", join("\n", sort(keys(%thrice))), "\n";

		diff_thrice($dbs1, $dbs2,
					$diffi, $diffi2, $row, $fselochsh, $bigh,
					$primfil, $mirrfil, $primhost, $mirrhost, 
					$primtmp, $mirrtmp, 
					$ppresh2, $mpresh2,
					$primsh, $mirrsh,
					\@aofiles, \@heapfiles, \@unknownfiles
			);

		exit(1);
	}

	print "\nenhanced diff [$floc]: no differences\n\n";

} # end diff_again

# MPP-8555: block by block diff 
sub diff_thrice
{
	my ($dbs1, $dbs2,
		$diffi, $diffi2, $row, $fselochsh, $bigh,
		$primfil, $mirrfil, $primhost, $mirrhost, 
		$primtmp, $mirrtmp,
		$ppresh2, $mpresh2,
		$primsh, $mirrsh, $aofiles, $heapfiles, $unknownfiles) = @_;

	my $bstat = 1;

	my (@allfiles1, @allfiles2);

	for my $fil (@{$aofiles})
	{
		my $eof = $fil->{eof};

		# calculate hash for files, but prefix hash with filename
		push @allfiles1, 
		$fil->{file} ."  ( perl MYPRESH21 -adler -ao  $eof  MYDIR/" . $fil->{file}
		. " ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil->{file} . 
		':ao: ", $_ ;}' . "'";
	}
	for my $fil (@{$heapfiles})
	{
		push @allfiles1, 
		$fil . "  ( perl MYPRESH21 -adler -heap  MYDIR/" . $fil
		. " ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil . 
		':heap: ", $_ ;}' . "'";
###			File::Spec->catfile($primfil, $fil->{file});
	}
	for my $fil (@{$unknownfiles})
	{
		push @allfiles1, 
		$fil . "  ( cat MYDIR/" . $fil
		. " | openssl dgst -md5 -hex ) 2>&1  "
		# should be: perl -e 'for (<STDIN>) { print "filename: ", $_ ;}'
		. "| perl -e 'for (<STDIN>) { print \"" . $fil . 
		':unknown: ", $_ ;}' . "'";
###			File::Spec->catfile($primfil, $fil->{file});
	}

#	print Data::Dumper->Dump([$dbs1]);
#	print Data::Dumper->Dump([$dbs2]);

	# need filename to sort correctly
	@allfiles2 = sort @allfiles1;
	@allfiles1 = ();

	# remove the filename sort key, leaving command string
	for my $fff (@allfiles2)
	{
		my @zzz = split(/\s/, $fff, 2);
		shift @zzz;
		push @allfiles1, $zzz[0];
	}


	my $numfiles = scalar(@allfiles1);
	my $floc = $row->{ploc};

	print "recheck $numfiles files for $floc\n";
	print Data::Dumper->Dump(\@allfiles1);

	$primsh  =~ s/deux/trois/;
	$mirrsh  =~ s/deux/trois/;
	$primtmp =~ s/deux/trois/;
	$mirrtmp =~ s/deux/trois/;

	# use openssl to construct an md5 hash for every file
	my $cmd_template = join ("\n", @allfiles1);
	my $cmd = $cmd_template;
	$cmd =~ s/MYHOST/$primhost/g;
	$cmd =~ s/MYDIR/$primfil/g;
	$cmd =~ s/MYOUT/$primtmp/g;
	$cmd =~ s/MYPRESH2/$ppresh2/g;

	my $fh;
	open ($fh, '>', $primsh);

	print $fh $cmd, "\n";

	close $fh;

	$cmd = $cmd_template;
	$cmd =~ s/MYHOST/$mirrhost/g;
	$cmd =~ s/MYDIR/$mirrfil/g;
	$cmd =~ s/MYOUT/$mirrtmp/g;
	$cmd =~ s/MYPRESH2/$mpresh2/g;

	open ($fh, '>', $mirrsh);

	print $fh $cmd, "\n";

	close $fh;

	# copy the shell scripts to the segments, execute them, then
	# copy back the results. Note that we add "1" as a suffix to
	# the script name and output file name to handle the case
	# where the master and segment are on the same machine (useful
	# for testing)

	my $cmd_template2 = 
			'gpscp -h MYHOST MYSHSCRIPT =:MYSHSCRIPT1 ; ' .
			'gpssh -h MYHOST \' sh MYSHSCRIPT1 > MYOUT  \' ; ' .
			'gpscp -h MYHOST  =:MYOUT MYOUT1 ; ' ;

	$cmd = $cmd_template2;
	$cmd =~ s/MYHOST/$primhost/g;
	$cmd =~ s/MYDIR/$primfil/g;
	$cmd =~ s/MYOUT/$primtmp/g;
	$cmd =~ s/MYSHSCRIPT/$primsh/g;

	print $cmd, "\n";

	system ($cmd);

	$cmd = $cmd_template2;
	$cmd =~ s/MYHOST/$mirrhost/g;
	$cmd =~ s/MYDIR/$mirrfil/g;
	$cmd =~ s/MYOUT/$mirrtmp/g;
	$cmd =~ s/MYSHSCRIPT/$mirrsh/g;

	print $cmd, "\n";

	system ($cmd);

	# add "1" suffix on master
	$primtmp .= '1';
	$mirrtmp .= '1';

	my $diffi3 = `diff -b $primtmp $mirrtmp`;

	{
		print "raw diff:\n";
		print $diffi;

		print "\nenhanced diff:\n";
		print $diffi2;

		if (length($diffi3))
		{
			print "\nblock by block diff:\n";
			print $diffi3;
		}
		else
		{
			print "\block by block diff [$floc]: no differences\n\n";
		}

		print Data::Dumper->Dump([$row]);
		
		print Data::Dumper->Dump([$bigh]);	 

		die "found differences";
		exit(1);
	}

	exit(0);

} # end diff_thrice

if (1)
{
    my $psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

	# select from gp_segment_configuration and pg_filespace_entry,
	# constructing a single row for each primary/mirror pair

    $psql_str .= " -c \' ".
		"select g.hostname||f.fselocation as fselocation, " .
		"g.port " .
		"from pg_filespace_entry f, gp_segment_configuration g " .
		"where f.fsedbid = 1 and g.dbid = 1  \' ";

    my $tabdef = `$psql_str`;
	
	my $fseloc = tablelizer($tabdef);

	# hash the file system entry location and mod it to an acceptable size
	my $fselochsh = perlhash($fseloc->[0]->{fselocation}) % 100000000000000;

	print "Hash for \"", $fseloc->[0]->{fselocation}, 
	"\" is  ", $fselochsh, "\n\n";

	# XXX XXX: port number!
	$glob_port = $fseloc->[0]->{port};

	$psql_str = "psql ";

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

	print $psql_str, "\n";

	$tabdef = `$psql_str`;

	print $tabdef;
	
	my $gp_seg_config = tablelizer($tabdef);

	$psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

	# checkpoint the database to flush pending writes
    $psql_str .= " -c \' checkpoint \'" ;

	$tabdef = `$psql_str`;

#	print $tabdef;

#	print Data::Dumper->Dump([$gp_seg_config]);

	my $presh_txt = get_presh_txt();
	my $presh2_txt = get_presh2_txt();

	my $ignore_arr = ignore_list($glob_ignore);

	my $ignore_cmd = join (" | ", @{$ignore_arr});

	for my $row (@{$gp_seg_config})
	{
		my $prim_dd = $row->{ploc};
		my $mirr_dd = $row->{mloc};

		my @foo = File::Spec->splitdir($prim_dd);
		my $fil = pop @foo;

		print "checking directory: ", $fil, "\n";

		my $primfil = $prim_dd;
		my $mirrfil = $mirr_dd;
		
		my $primhost = $row->{phostname};
		my $mirrhost = $row->{mhostname};

		my $primtmp = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . ".prim");
		my $mirrtmp = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . ".mirr");

		my $ppresh = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_ppre.pl");
		my $mpresh = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_mpre.pl");

		my $ppresh2 = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_ppre2.pl");
		my $mpresh2 = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_mpre2.pl");

		my $primsh = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_prim.sh");
		my $mirrsh = File::Spec->catfile("/tmp", 
										  $fselochsh . "_" . $fil . 
										 "_mirr.sh");

		# use openssl to construct an md5 hash for every file
		# Note: use mypresh*1*, because this script runs on the copy
		# of the file on the segment
		my $cmd_template = 
			'find MYDIR -type f | xargs perl MYPRESH1  ' .
			'| ' . $ignore_cmd . " " .
			'| sort  ' .
			'| perl -i -ple \'s|MYDIR||gm \' ' . 
			# strip out [hostname] prefix
			'| perl -i -ple \'s|^\\s*\\[.*\\]\\s*||gm \'  ' ;

		my $cmd = $cmd_template;
		$cmd =~ s/MYHOST/$primhost/g;
		$cmd =~ s/MYDIR/$primfil/g;
		$cmd =~ s/MYOUT/$primtmp/g;
		$cmd =~ s/MYPRESH/$ppresh/g;

		# build local shell scripts for the commands

		my $fh;

		open ($fh, '>', $ppresh);
		
		print $fh $presh_txt;

		close $fh;

		open ($fh, '>', $ppresh2);
		
		print $fh $presh2_txt;

		close $fh;

		open ($fh, '>', $primsh);

		print $fh $cmd, "\n";

		close $fh;

		$cmd = $cmd_template;
		$cmd =~ s/MYHOST/$mirrhost/g;
		$cmd =~ s/MYDIR/$mirrfil/g;
		$cmd =~ s/MYOUT/$mirrtmp/g;
		$cmd =~ s/MYPRESH/$mpresh/g;

		open ($fh, '>', $mpresh);
		
		print $fh $presh_txt;

		close $fh;

		open ($fh, '>', $mpresh2);
		
		print $fh $presh2_txt;

		close $fh;

		open ($fh, '>', $mirrsh);

		print $fh $cmd, "\n";

		close $fh;

		# copy the shell scripts to the segments, execute them, then
		# copy back the results. Note that we add "1" as a suffix to
		# the script name and output file name to handle the case
		# where the master and segment are on the same machine (useful
		# for testing)

		my $cmd_template2 = 
			'gpscp -h MYHOST MYSHSCRIPT =:MYSHSCRIPT1 ; ' .
			'gpscp -h MYHOST MYPRESCRIPT =:MYPRESCRIPT1 ; ' .
			'gpscp -h MYHOST MYPRE2SCRIPT =:MYPRE2SCRIPT1 ; ' .
			'gpssh -h MYHOST \' sh MYSHSCRIPT1 > MYOUT  \' ; ' .
			'gpscp -h MYHOST  =:MYOUT MYOUT1 ; ' ;

		$cmd = $cmd_template2;
		$cmd =~ s/MYHOST/$primhost/g;
		$cmd =~ s/MYDIR/$primfil/g;
		$cmd =~ s/MYOUT/$primtmp/g;
		$cmd =~ s/MYSHSCRIPT/$primsh/g;
		$cmd =~ s/MYPRESCRIPT/$ppresh/g;
		$cmd =~ s/MYPRE2SCRIPT/$ppresh2/g;

		print $cmd, "\n";


		my $time1 = time();
		system ($cmd);
		my $time2 = time();

		my $numfiles = `wc -l < $primtmp`;
		chomp $numfiles;

		print "\nprimary: ", $numfiles, " files, ", $time2-$time1," seconds\n\n";

		$cmd = $cmd_template2;
		$cmd =~ s/MYHOST/$mirrhost/g;
		$cmd =~ s/MYDIR/$mirrfil/g;
		$cmd =~ s/MYOUT/$mirrtmp/g;
		$cmd =~ s/MYSHSCRIPT/$mirrsh/g;
		$cmd =~ s/MYPRESCRIPT/$mpresh/g;
		$cmd =~ s/MYPRE2SCRIPT/$mpresh2/g;

		print $cmd, "\n";

		$time1 = time();
		system ($cmd);
		$time2 = time();

		$numfiles = `wc -l < $mirrtmp`;
		chomp $numfiles;

		print "\nmirror: ", $numfiles, " files, ", $time2-$time1," seconds\n\n";

		# add "1" suffix on master
		$primtmp .= '1';
		$mirrtmp .= '1';

		my $diffi = `diff -b $primtmp $mirrtmp`;

		if (!length($diffi))
		{
			print "\n$fil: no differences\n\n";
		}
		else
		{
			my $bigh = dandydiff($primtmp, $mirrtmp);

			if (exists($bigh->{extrafiles})
				&& scalar(@{($bigh->{extrafiles})}))
			{
				print $diffi;

				print Data::Dumper->Dump([$row]);

				print Data::Dumper->Dump([$bigh]);	 

				die "found extra files in data directory";
				exit (1);
			}

			print "\n\nraw diff mismatch for $fil: will diff again using storage-specific techniques\n\n";

			my ($dbs1, $dbs2);

			if (exists($bigh->{sizediff}) 
				&& scalar(keys(%{($bigh->{sizediff})})))
			{
				$dbs1 = resolve_sizediff($diffi, $row, $fselochsh, $bigh);
			}
			if (exists($bigh->{hashdiff})
				&& scalar(keys(%{($bigh->{hashdiff})})))
			{
				$dbs2 = resolve_hashdiff($diffi, $row, $fselochsh, $bigh);
			}

			diff_again($dbs1, $dbs2,
					   $diffi, $row, $fselochsh, $bigh,
					   $primfil, $mirrfil, $primhost, $mirrhost, 
					   $primtmp, $mirrtmp, 
					   $ppresh2, $mpresh2,
					   $primsh, $mirrsh);
		}

	}
	
	print "no differences found\n";
	exit(0);

}


exit();
