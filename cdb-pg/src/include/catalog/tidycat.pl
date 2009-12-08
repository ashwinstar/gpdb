#!/usr/bin/perl
#
# $Header$
#
# copyright (c) 2009, 2010
# Author: Jeffrey I Cohen
#
#
use POSIX;
use Pod::Usage;
use Getopt::Long;
use Data::Dumper;
use strict;
use warnings;

=head1 NAME

B<tidycat.pl> - generate catalog entries

=head1 SYNOPSIS

B<ct> [options] 

Options:

    -help            brief help message
    -man             full documentation
    -preview         show data file updates, but don't modify the files

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-preview>

    Show data file updates, but don't modify the files.

=back

=head1 DESCRIPTION

tidycat.pl handles all of your stinky catalog problems, leaving a
fresh, clean scent.

=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2009, 2010 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

my $glob_id = "";

my $glob_platform;
my $glob_tabwidth = 4;
my $glob_faketab = 0;
my $glob_tabstr = "\t";
#my $glob_tabstr = " " x $glob_tabwidth; 
my $glob_preview;
my $glob_tmpdir = "/tmp";

BEGIN {
    my $man  = 0;
    my $help = 0;
	my $preview = 0;

    GetOptions(
               'help|?' => \$help, man => \$man, 
	 	       "preview|pretend" => \$preview
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

	$glob_preview = $preview;

#    print "loading...\n" ;
}

# DO NOT extend this list!  To ensure smooth upgrade, all new tables
# must have a static reltype_oid
my %dynamic_reltype_h = 
	(
 5000 => "gp_configuration",
 5002 => "gp_distribution_policy",
 5001 => "gp_id",
 5008 => "gp_master_mirroring",
 5003 => "gp_version_at_initdb",
 2600 => "pg_aggregate",
 2601 => "pg_am",
 2602 => "pg_amop",
 2603 => "pg_amproc",
 6105 => "pg_appendonly",
 2604 => "pg_attrdef",
 1261 => "pg_auth_members",
 1260 => "pg_authid",
 2605 => "pg_cast",
 2606 => "pg_constraint",
 2607 => "pg_conversion",
 1262 => "pg_database",
 2608 => "pg_depend",
 2609 => "pg_description",
 6040 => "pg_exttable",
 2610 => "pg_index",
 2611 => "pg_inherits",
 2612 => "pg_language",
 2613 => "pg_largeobject",
 2614 => "pg_listener",
 2615 => "pg_namespace",
 2616 => "pg_opclass",
 2617 => "pg_operator",
 5010 => "pg_partition",
 5011 => "pg_partition_rule",
 1136 => "pg_pltemplate",
 6026 => "pg_resqueue",
 2618 => "pg_rewrite",
 1214 => "pg_shdepend",
 2396 => "pg_shdescription",
 2619 => "pg_statistic",
 1213 => "pg_tablespace",
 2836 => "pg_toast_1255",
 2842 => "pg_toast_1260",
 2844 => "pg_toast_1262",
 2846 => "pg_toast_2396",
 2830 => "pg_toast_2604",
 2832 => "pg_toast_2606",
 2834 => "pg_toast_2609",
 2838 => "pg_toast_2618",
 2840 => "pg_toast_2619",
 2620 => "pg_trigger",
 5004 => "pg_window"
	);

sub getcomment1
{
	my $tname = shift;

	my $bigstr = <<"EOF_bigstr";
/* ----------------
 *TWOTABSTABLENAME definition.  cpp turns this into
 *TWOTABStypedef struct FormData_TABLENAME
 * ----------------
 */
EOF_bigstr

my $twotabs = $glob_tabstr x 2;

$bigstr =~ s/TWOTABS/$twotabs/gm;
$bigstr =~ s/TABLENAME/$tname/gm;

return $bigstr;

}

sub getcomment2
{
	my $tname = shift;

	my $bigstr = <<"EOF_bigstr";
/* ----------------
 *TWOTABSForm_TABLENAME corresponds to a pointer to a tuple with
 *TWOTABSthe format of TABLENAME relation.
 * ----------------
 */
EOF_bigstr

my $twotabs = $glob_tabstr x 2;

$bigstr =~ s/TWOTABS/$twotabs/gm;
$bigstr =~ s/TABLENAME/$tname/gm;

return $bigstr;

}
sub getcomment3
{
	my $tname = shift;

	my $bigstr = <<"EOF_bigstr";
/* ----------------
 *TWOTABScompiler constants for TABLENAME
 * ----------------
 */
EOF_bigstr

my $twotabs = $glob_tabstr x 2;

$bigstr =~ s/TWOTABS/$twotabs/gm;
$bigstr =~ s/TABLENAME/$tname/gm;

return $bigstr;

}

sub sqltype_to_ctype
{
	my $coltype = shift;
	my $ctype;

	return $coltype
		unless ($coltype =~ m/^(name|oid|boolean|real|smallint|integer)$/);

	$ctype = "NameData" if ($coltype =~ m/name/);
	$ctype = "Oid" if ($coltype =~ m/oid/);
	$ctype = "bool" if ($coltype =~ m/boolean/);
	$ctype = "float4" if ($coltype =~ m/real/);
	$ctype = "int2" if ($coltype =~ m/smallint/);
	$ctype = "int4" if ($coltype =~ m/integer/);

	return $ctype;
}

sub ctype_to_btree_op_prefix
{
	my $ctype = shift;

	return $ctype
		unless ($ctype =~ m/Oid|NameData/);

	$ctype = "name" if ($ctype =~ m/NameData/);
	$ctype = "oid" if ($ctype =~ m/Oid/);

	return $ctype;
}

sub parseindex
{
	my ($alltabs, $bigstr) = @_;
	my $bUnique = 0;

	$bUnique = ($bigstr =~ m/create\s+unique\s+index/i);

	$bigstr =~ s/^\s*create\s+(unique\s+)?index\s+on//i;

#	print "f:",$bigstr,":f\n";

#	my @foo = ($bigstr =~ m/^s*(\w+)\s*\((.*)\)\s*(with\s*\(.*\))?/im);
#	my @foo = ($bigstr =~ m/\s*(\w+)\s*\(((?<!\)).*\))/i);
#   tablename (col1 (, col2)) 
	my @foo = ($bigstr =~ m/\s*(\w+)\s*\((\w+(\s*\,\s*\w+)*)\)/i);

#	print Data::Dumper->Dump(\@foo), "\n";

	die "bad index def: $bigstr" unless (2 < scalar(@foo));

	my $tname = shift @foo;
	my $colnamlist = shift @foo;
	my @cols = split(",", $colnamlist);

	my $icols = [];

	for my $c1 (@cols)
	{
		$c1 = lc($c1);
		$c1 =~ s/^\s+//;
		$c1 =~ s/\s+$//;

		die "no such col: $c1" 
			unless (exists($alltabs->{$tname}->{colh}->{$c1}));

		my $colop = $alltabs->{$tname}->{colh}->{$c1};

		$colop = ctype_to_btree_op_prefix($colop);
		$colop = $colop . "_ops";

		push @{$icols}, [$c1, $colop];
	}

	@foo = split(/\)/, $bigstr, 2);

	my $with = pop @foo;

	# XXX XXX: FIX THIS!!!
	my @baz = ($with =~ m/(\d+)/);

	die "no index oid: $bigstr" unless (scalar(@baz));

	my $indoid = shift @baz;

	$tname = lc($tname);
	$tname =~ s/^\s+//;
	$tname =~ s/\s+$//;

	die "no such table: $tname" unless (exists($alltabs->{$tname}));

	$alltabs->{$tname}->{indexes} = []
		unless (exists(	$alltabs->{$tname}->{indexes}));
	
	my $idef = {unique => $bUnique};
	$idef->{cols} = $icols;
	$idef->{indexid} = $indoid;

	push @{$alltabs->{$tname}->{indexes}}, $idef;
}


sub parsecols
{
	my ($alltabs, $tname, $bigstr) = @_;

	$bigstr =~ s/^\s*\(//s;
	$bigstr =~ s/\)\s*$//s;

	my @foo = split(/\n/, $bigstr);

	my $collist = [];
	my $ii = 0;

	my $precomment;

	my $colh = {};

	for my $lin (@foo)
	{
		unless (length($lin))
		{
			# mark the blank lines (remove this later)
			$precomment .= "\n**TK_BLANK_LINE**";
			next;
		}

		if (($lin =~ m/^\s*\-\-/) || ($lin =~ m/^\s+$/))
		{
			$precomment = ""
				unless (defined($precomment));

			chomp($lin);
			$precomment .= "\n" . $lin;
		}
		else
		{
			die "bad col: $lin"
				unless ($lin =~ m/\s*\w+\s+\w+/);

			my @baz = ($lin =~ m/\s*(\w+)\s+(\w+)\s*(\,)?\s*(\-\-.*)?/);

#			print Data::Dumper->Dump(\@baz), "\n";

			my $colname = lc (shift @baz );
			my $coltype = lc (shift @baz );

			die ("duplicate colname: $colname for $tname")
				if (exists($colh->{$colname}));

			shift @baz ; # comma or undef

			my $postcomment;

			$postcomment = $baz[0] if (scalar(@baz) && defined($baz[0]));
			
			my $coldef = {colname => $colname, sqltype => $coltype};

			my $ctype = sqltype_to_ctype($coltype);
			$coldef->{ctype} = $ctype;

			$colh->{$colname} = $ctype;

			$coldef->{postcomment} = $postcomment if (defined($postcomment));
			$coldef->{precomment} = $precomment if (defined($precomment));

			push @{$collist}, $coldef;

			$ii++;
			undef $precomment;
		}

	}
	
	# add the Oid column
	if ($alltabs->{$tname}->{with}->{oid})
	{
		die "name conflict: cannot have named oid column"
			if (exists($colh->{oid}));

		$colh->{oid} = "Oid";
	}

	$alltabs->{$tname}->{cols} = $collist;
	$alltabs->{$tname}->{colh} = $colh;

#	print Data::Dumper->Dump(\@foo), "\n";

} # end parsecols

sub parsetab
{
	my ($alltabs, $bigstr) = @_;
	
	my $tabdef = $bigstr;

	$bigstr =~ s/create\s+table//is;
	$bigstr =~ s/^\s+//s;
	$bigstr =~ s/\s+$//s;

	my @foo = ($bigstr =~ m/^\s*(\w+)/s);

	die "no tablename: $tabdef" unless (scalar(@foo));

	my $tname = shift @foo;

	$bigstr =~ s/$tname//s;
	$bigstr =~ s/^\s+//s;

	$tname =~ s/^\s+//;
	$tname =~ s/\s+$//;
	$tname = lc($tname);

	$alltabs->{$tname} = {tabdef_text => $tabdef};

	if ($bigstr =~ m/\s*with\s*\(.*\)/is)
	{
		# match a "WITH (...)" followed by "(" 
		@foo = ($bigstr =~ m/^\s*(with\s*\(.*\)(?=(\s*\()))/is);

		die "bad with: $bigstr" unless (scalar(@foo));

		my $withclause  = shift @foo;

		$alltabs->{$tname}->{with} = {text => $withclause};

		$bigstr =~ s/\s*(with\s*\(.*\)(?=(\s*\()))//is;
		$bigstr =~ s/^\s+//s;

		$withclause =~ s/^\s*with\s*\(//is;
		$withclause =~ s/\)\s*$//s;

		my @foo = split(",", $withclause);

		for my $withdef (@foo)
		{
			my @baz = split("=", $withdef, 2);

			die "bad with def: $withdef" unless (2 == scalar(@baz));

			my $kk = shift @baz;
			my $vv = shift @baz;

			$kk =~ s/^\s+//;
			$kk =~ s/\s+$//;
			$kk = lc($kk);

			$vv =~ s/^\s+//;
			$vv =~ s/\s+$//;

			$alltabs->{$tname}->{with}->{$kk} = $vv;

		}
		
		die "no relid for table $tname"
			unless (exists($alltabs->{$tname}->{with}->{relid}));

		if (exists($alltabs->{$tname}->{with}->{shared}))
		{
			$alltabs->{$tname}->{with}->{shared} =
				($alltabs->{$tname}->{with}->{shared} =~ m/t|true|y|yes|1/i);
		}
		else
		{
			$alltabs->{$tname}->{with}->{shared} = 0;
		}

		if (exists($alltabs->{$tname}->{with}->{oid}))
		{
			$alltabs->{$tname}->{with}->{oid} =
				($alltabs->{$tname}->{with}->{oid} =~ m/t|true|y|yes|1/i);
		}
		else
		{
			$alltabs->{$tname}->{with}->{oid} = 1;
		}

		# build a camel-case string for the tablename
		unless (exists($alltabs->{$tname}->{with}->{camelcase}))
		{
			my $cc1 = $tname;

			if ($cc1 =~ m/^(pg|gp)\_/i)
			{
				# remove prefix
				$cc1 =~ s/^...//;
			}
			$alltabs->{$tname}->{with}->{camelcase} = ucfirst($cc1);
		}

		if (exists($alltabs->{$tname}->{with}->{reltype_oid}))
		{
			my $relid = $alltabs->{$tname}->{with}->{relid};

			die "table $tname has a dynamic reltype oid -- cannot redefine to static!!"
				if (exists($dynamic_reltype_h{$relid}));
		}
		else
		{
			my $relid = $alltabs->{$tname}->{with}->{relid};

			die "table $tname must have a static reltype oid"
				unless (exists($dynamic_reltype_h{$relid}));
		}

	} # end if with

	parsecols($alltabs, $tname, $bigstr);

#	print "bigstr: ", $bigstr, "\n";
	

} # end parsetab


sub parsetabdef
{
	my ($alltabs, $bigstr) = @_;

	my @statements = split(';', $bigstr);

#	print Data::Dumper->Dump(\@statements), "\n";

	for my $stat (@statements)
	{
		if ($stat =~ m/^\s*create\s+table/is)
		{
			parsetab($alltabs, $stat);
		}
		elsif ($stat =~ m/^\s*create\s+(unique\s+)?index/is)
		{
			parseindex($alltabs, $stat);
		}
	}

} # end parsetabdef

# expects val1, len1, val2, len2
# where length values are based on printed offset, not length(val), ie
# embedded tabs are counted.
sub tabalign
{
	my ($tabwidth, $collist) = @_;
	my $tabstr = $glob_tabstr;

	my $maxlen = 0;
	for my $coldef (@{$collist})	
	{
		die "bad coldef: " . Data::Dumper->Dump([$coldef])
			unless (scalar(@{$coldef}) > 3);

		$maxlen = $coldef->[1] if ($coldef->[1] > $maxlen);
	}

	# find the tab position for the second column
	my $col2tab = (POSIX::ceil($maxlen / $tabwidth)) * $tabwidth;

	$col2tab++ if ($col2tab == $maxlen);

#	print $maxlen, " " , $col2tab, "\n";

#	print Data::Dumper->Dump($collist), "\n";

	for my $ii (0..(scalar(@{$collist})-1))
	{
#		print Data::Dumper->Dump($collist->[$ii]), "\n";

		my $val1 = shift @{$collist->[$ii]};
		my $len1 = shift @{$collist->[$ii]};
		my $val2 = shift @{$collist->[$ii]};
		my $len2 = shift @{$collist->[$ii]};

		my $newval = $val1;

		if ($len1 < $col2tab)
		{
			my $mod1 = $len1 % $tabwidth;

#			print "mod: $mod1\n";

			if ($mod1)
			{
				$len1 += ($tabwidth - $mod1);
				if ($glob_faketab)
				{
					$val1 .= " " x ($tabwidth - $mod1);
				}
				else
				{
					$val1 .= $tabstr;
				}
			}
		}

		while ($len1 < $col2tab)
		{
			$len1 += $tabwidth;
			$val1 .= $tabstr;
		}

#		unshift @{$collist->[$ii]}, $len1 + $len2 + $tabwidth;
#		unshift @{$collist->[$ii]}, $val1 . $tabstr . $val2;		
		unshift @{$collist->[$ii]}, $len1 + $len2 ;
		unshift @{$collist->[$ii]}, $val1 . $val2;		
	} # end for ii

#	print Data::Dumper->Dump($collist), "\n";

	return $collist;
}

sub simpletabalign
{
	my ($col1, $col2) = @_;

	my $colitem = [];

	push @{$colitem}, $col1;
	push @{$colitem}, length($col1);

	push @{$colitem}, $col2;
	push @{$colitem}, length($col2);

	my $collist = tabalign($glob_tabwidth, [$colitem]);

	return $collist->[0]->[0];
}

sub formatcolconst
{
	my ($tnamepref, $allcol) = @_;

	my $collist = [];

	{
		my $colitem = [];
		my $cdef;

		$cdef = "#define Natts_" . $tnamepref;

		push @{$colitem}, $cdef;
		push @{$colitem}, length($cdef) ;
		push @{$colitem}, scalar(@{$allcol});
		push @{$colitem}, length(scalar(@{$allcol}));
			
		push @{$collist}, $colitem;
	}

	my $ii = 1;

	for my $coldef (@{$allcol})
	{
		my $colitem = [];
		my $cdef;

		$cdef = "#define Anum_" . $tnamepref . "_" . $coldef->{colname};

		push @{$colitem}, $cdef;
		push @{$colitem}, length($cdef) ;

		push @{$colitem}, $ii;
		push @{$colitem}, length($ii);

		$ii++;
			
		push @{$collist}, $colitem;
	}

	$collist = tabalign($glob_tabwidth, $collist);

	my $bigstr = "";

	for my $coldef (@{$collist})
	{
		my $st1 = shift @{$coldef};

		$bigstr .= $st1 . "\n";
	}

	return $bigstr;

} # end formatcolconst

sub formatcols
{
	my $allcol = shift;

	my $collist = [];

	my $bigstr = "";

	# formatting trick: col1 is prefixed by a tab, so increase its
	# length by tabwidth.  col2 is the column name followed by a
	# semicolon.  And col3 is the optional comment.  Build an array of
	# items of [val1, len1, val2, len2, val3, len3]
	#
	# tabalign will shift off the first 4 entries and append the values
	# with embedded tabs to align them, nicely, then unshift the
	# append values and combined length back into original array,
	# resulting in:
	#  ["val1<tabs>val2", len1+<tabs>+len2, val3, len3]
	#
	# So now we just feed this array back into tabalign to get:
	#  ["val1<tabs>val2<tabs>val3", len1+<tabs>+len2+<tabs>+len3]

	for my $coldef (@{$allcol})
	{
		my $colitem = [];

		push @{$colitem}, $glob_tabstr . $coldef->{ctype};
		push @{$colitem}, length($coldef->{ctype}) + $glob_tabwidth;

		push @{$colitem}, $coldef->{colname} . ";";
		push @{$colitem}, length($coldef->{colname}) + 1;

		if (exists($coldef->{postcomment}))
		{
			my $pc = $coldef->{postcomment};

			$pc =~ s/^\-\-/\/\*/;
			$pc .= " \*\/";

			push @{$colitem}, $pc;
			push @{$colitem}, length($pc);
		}
		else
		{
			push @{$colitem}, "";
			push @{$colitem}, 0;
		}

		push @{$collist}, $colitem;
	}

	$collist = tabalign($glob_tabwidth, $collist);
	$collist = tabalign($glob_tabwidth, $collist);

	for my $ii (0..(scalar(@{$collist})-1))
	{
		my $coldef1 = $allcol->[$ii];
		my $coldef2 = $collist->[$ii];
		my $st1 = shift @{$coldef2};

		if (1 && (exists($coldef1->{precomment})))
		{
			my $pc = $coldef1->{precomment};
			my $bComment = 0;

			$bComment = ($pc =~ m/^\s*\-\-/m);

			if ($bComment)
			{
				# make everything into a single-line comment
				$pc =~ s/^\s*\-\-(.*)/$glob_tabstr\/\*$1 \*\//gm;

				# find adjacent single-line comments by looking for
				# "*/\n/*" and merge them together
				$pc =~ s/\*\/\s*(\n\s*)\/\*/$1 \*/gm;

				$pc .= "\n";
			}

			# remove the blank lines
			$pc =~ s/\*\*TK\_BLANK\_LINE\*\*//gm;

			$bigstr .= $pc;
		}

		$bigstr .= $st1 . "\n";
	}

	return $bigstr;

} # end formatcols


sub fix_issharedrelation_function
{
	my $alltabs = shift;
	my $bigstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		next
			unless ($vv->{with}->{shared});

		$bigstr .= "\n\n";
		$bigstr .= "/* relation id: $relid - $kk */\n";		
		$bigstr .= "relationId == " . $vv->{CamelCaseRelationId} . " || ";
		$bigstr .= "\n";
	}

	my $indstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		next
			unless ($vv->{with}->{shared});

		next unless (exists($vv->{indexes}));

		$indstr .= "\n\n";
		for my $ind (@{$vv->{indexes}})
		{
			$indstr .= "/* relation id: $relid - $kk */\n";		
			$indstr .= "relationId == " . $ind->{CamelCaseIndexId} . " || " ;
			$indstr .= "\n";
		}
		$indstr .= "\n\n";
	}

	return [$bigstr, $indstr];
}

sub bootparse_str
{
	my ($ccrelid, $ucreltypid) = @_;

	my $bigstr = <<"EOF_bigstr";
SEVENTABScase CAMELCASERELATIONID:
EIGHTTABStypid = UPPERCASERELTYPID;
EIGHTTABSbreak;
EOF_bigstr

	my $seventabs = $glob_tabstr x 7;
	my $eighttabs = $glob_tabstr x 8;

	$bigstr =~ s/SEVENTABS/$seventabs/gm;
	$bigstr =~ s/EIGHTTABS/$eighttabs/gm;

	$bigstr =~ s/CAMELCASERELATIONID/$ccrelid/gm;
	$bigstr =~ s/UPPERCASERELTYPID/$ucreltypid/gm;

	return $bigstr;
}

sub fix_bootparse
{
	my $alltabs = shift;
	my $bigstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		next
			unless (exists($vv->{with}->{reltype_oid}));

		$bigstr .= "\n\n";
		$bigstr .= "/* relation id: $relid - $kk */\n";		
		$bigstr .=  bootparse_str($vv->{CamelCaseRelationId}, 
								  $vv->{UppercaseReltypeOid});
		$bigstr .= "\n\n";
	}

	return $bigstr;

}

sub formattypedata
{
	my ($oid, $tname, $reltype_oid) = @_;

	my $bigstr = <<'EOF_bigstr';
DATA(insert OID = RELTYPE_OID ( TABLENAME	    PGNSP PGUID -1 f c t \054 THEE_OID 0 record_in record_out record_recv record_send - d x f 0 -1 0 _null_ _null_ ));	
EOF_bigstr

$bigstr =~ s/TABLENAME/$tname/gm;
$bigstr =~ s/RELTYPE\_OID/$reltype_oid/gm;
$bigstr =~ s/THEE\_OID/$oid/gm;

return $bigstr;

}

sub formattypes
{
	my $alltabs = shift;
	my $bigstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		next
			unless (exists($vv->{with}->{reltype_oid}));

		my $reltype_oid = $vv->{with}->{reltype_oid};

		my $uc_tname = uc($kk);

		$vv->{UppercaseReltypeOid} = $uc_tname . "_RELTYPE_OID";

		$bigstr .= "/* relation id: $relid - $kk */\n";		
		$bigstr .= formattypedata($relid, $kk, $reltype_oid);
		$bigstr .= 
			"#define " . $uc_tname . "_RELTYPE_OID " . $reltype_oid . "\n";
	}

	return $bigstr;
}
sub formatindexes
{
	my $alltabs = shift;
	my $bigstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		next unless (exists($vv->{indexes}));

		for my $ind (@{$vv->{indexes}})
		{
			my $iname;
			my $icolist;
			my $camelname;

			$iname = $kk;
			$icolist = "";
			$camelname = $vv->{with}->{camelcase};

			for my $cols (@{$ind->{cols}})
			{
				$iname .= "_" . $cols->[0];
				$camelname .= ucfirst($cols->[0]);

				if (length($icolist))
				{
					$icolist .= ", ";
				}
				$icolist .= $cols->[0] . " " . $cols->[1];
			}
			if ($ind->{unique})
			{
				$bigstr .= "/* relation id: $relid - $kk */\n";
				$bigstr .= "DECLARE_UNIQUE_INDEX(";
			}
			else
			{
				$bigstr .= "/* relation id: $relid - $kk */\n";
				$bigstr .= "DECLARE_INDEX(";
			}
			my $indexid = $ind->{indexid};

			$bigstr .=
				$iname . ", " . "$indexid, on " . $kk . 
				" using btree(" . $icolist . "));\n";

			$ind->{CamelCaseIndexId} = $camelname . "IndexId";

			my $ccdef = "#define " . $camelname . "IndexId";

			$bigstr .= simpletabalign($ccdef, $indexid) . "\n\n";

		}
	}

	return $bigstr;
}

sub formattabs
{
	my $alltabs = shift;
	my $bigstr = "";

	for my $kk (sort (keys (%{$alltabs})))
	{
		my $vv = $alltabs->{$kk};
		my $relid = $vv->{with}->{relid};

		$bigstr .= "\n/*\n TidyCat Comments for $kk:\n";
		$bigstr .= "  Table is shared, so catalog.c:IsSharedRelation is " .
			"updated.\n" if $vv->{with}->{shared};
		if ($vv->{with}->{oid})
		{
			$bigstr .= "  Table has an Oid column.\n";
		}
		else
		{
			$bigstr .= "  Table does not have an Oid column.\n";
		}
		if (exists($vv->{with}->{reltype_oid}))
		{
			$bigstr .= "  Table has static type (see pg_types.h).\n"; 
		}
		else
		{
			$bigstr .= "  Table does not have static type " .
				"(only legal for pre-3.3 tables). \n"; 
		}
		$bigstr .= "\n*/\n\n";


		$bigstr .= getcomment1($kk);

		$vv->{CamelCaseRelationId} = $vv->{with}->{camelcase} . "RelationId" ;

		my $ccdef = '#define ' . $vv->{with}->{camelcase} . "RelationId" ;

		$bigstr .= simpletabalign($ccdef, $relid) . "\n\n";
			
		$bigstr .= 'CATALOG('. $kk . ',' . $relid . ')';
		$bigstr .= " BKI_SHARED_RELATION" if $vv->{with}->{shared};
		$bigstr .= " BKI_WITHOUT_OIDS" unless $vv->{with}->{oid};
		$bigstr .= "\n{";

		$bigstr .= formatcols($vv->{cols});

		$bigstr .= "} FormData_" . $kk . ";\n";


		$bigstr .= "\n\n";

		$bigstr .= getcomment2($kk);

		$bigstr .= "typedef FormData_" . $kk . " *Form_" . $kk. ";\n";


		$bigstr .= "\n\n";

		$bigstr .= getcomment3($kk);

		$bigstr .= formatcolconst($kk, $vv->{cols});

	}

	return $bigstr;

} # end formattabs


sub parsefile
{
	my ($file_name, $finddefs) = @_;

	# if not looking for tidycat definitions, just find the code gen
	# sections

	my $begin_def_str = "TIDYCAT_BEGIN_CODEGEN";
	my $end_def_str = "TIDYCAT_END_CODEGEN";
	
	if (defined($finddefs))
	{
		$begin_def_str = "TIDYCAT_BEGINDEF";
		$end_def_str = "TIDYCAT_ENDDEF";
	}


	my $entrylist = [];

	my $bigstr ;

	open my $file_in, "< $file_name" or die "cannot open $file_name: $!";

	my $phase = "begindef"; # begindef, enddef, begingen, endgen

	my $ii = 0;

	my $entry;

	for my $ini (<$file_in>)
	{
		$ii++;
		if ($phase =~ m/enddef/)
		{
			if ($ini =~ m/$end_def_str/)
			{
				if ($ini =~ m/\*\//)
				{
					# look for trailing "*/" 
					$entry->{enddef} = $ii;
				}
				else
				{
					# probably the next line
					$entry->{enddef} = $ii + 1;
				}

				$entry->{def} = $bigstr;

				if (defined($finddefs))
				{
					$phase = "begingen";
				}
				else
				{
					# save the current entry
					push @{$entrylist}, $entry;
					$phase = "begindef"; 
				}

				next;
			}

			# append to the string!!
			$bigstr .= $ini;

		}
		elsif ($phase =~ m/begindef/)
		{
			next unless ($ini =~ m/$begin_def_str/);

			$entry = {begindef => $ii};
			$bigstr = "";
			$phase = "enddef";

			next;
		}
		elsif ($phase =~ m/begingen/)
		{
			# might have new definition and no codegen
			if ($ini =~ m/$begin_def_str/)
			{
				push @{$entrylist}, $entry;

				$entry = {begindef => $ii};
				$bigstr = "";
				$phase = "enddef";

				next;

			}
			elsif ($ini =~ m/TIDYCAT\_BEGIN\_CODEGEN/)
			{
				$entry->{begin_codegen} = $ii;
				$phase = "endgen";

			}
			else
			{
				# stop looking if no generated code within 3 lines...

				if (($ii - $entry->{enddef}) > 3)
				{
					# save the current entry
					push @{$entrylist}, $entry;
					$phase = "begindef"; 
				}
				next;
			}

		}
		elsif ($phase =~ m/endgen/)
		{
			if ($ini =~ m/TIDYCAT\_END\_CODEGEN/)
			{
				$entry->{end_codegen} = $ii;

				# save the current entry
				push @{$entrylist}, $entry;


				$bigstr = "";
				$phase = "begindef";

				
			}
			next;


		}

	} # end for ini

	die "unterminated tidycat definition in $file_name at " .
		$entry->{begindef} if (defined($entry) && ($phase =~ m/enddef/));

	if (defined($entry) && ($phase =~ m/begingen/))
	{
		push @{$entrylist}, $entry;
	}

	close $file_in;

	return $entrylist;
} # end parsefile

sub bufferfile
{
	my $file_name = shift;

	open my $file_in, "< $file_name" or die "cannot open $file_name: $!";

	my $buflist = [];

	for my $ini (<$file_in>)
	{
		push @{$buflist}, $ini;
	}
	close $file_in;

	return $buflist;
}

sub fixup_generated_code
{
	my ($basename, $fullname, $tmpname, $new_gen_list, $verzion, $nnow) = @_;

	my $elist_index = 1;

	# catalog.c index case is second set of definitions
	$elist_index = 2
		if ($basename =~ m/catalog\.c ind/);

	return
		unless (scalar(@{$new_gen_list}));

	my $elist = parsefile($fullname);
	my $buffil = bufferfile($fullname);

	print $basename,":\n", Data::Dumper->Dump($elist), "\n";

	return
		unless (scalar(@{$elist}) >= $elist_index);

	my $entry = $elist->[$elist_index - 1];

	my $bigstr = $entry->{def};

	my $genstr = "Generated by " . $verzion;

	$bigstr =~ s/Generated by.*tidycat\.pl\s+version\s+\d+\./$genstr/m;

	$bigstr =~ 
s/($genstr)\n(\s+on\s+)(\w+\s+\w+\s+\d+\s+\d+\:\d+\:\d+\s+\d+)/$1\n$2$nnow/m;

	$bigstr .= "\n" . join("", @{$new_gen_list});

	if (exists($entry->{begindef}))
	{
		splice (@{$buffil}, 
				($entry->{begindef}),
				($entry->{enddef} - $entry->{begindef})-1,
				$bigstr);
	}

	# write the modified file to the temp directory
	open my $file_out, "> " . $tmpname
		or die "Cannot open $tmpname for write";

	# write the modified file to the tmpdir
	print $file_out join("", @{$buffil});
	
	close $file_out;


} # end fixup_generated_code

if (1)
{
# 5000 table
# 6000 indexes

    my $verzion = "unknown";
	
	if (q$Revision$ =~ /\d+/)
    {
        $verzion = do { my @r = (q$Revision$ =~ /\d+/g); sprintf "%d."."%02d" x $#r, @r }; # must be all one line, for MakeMaker
    }

	$verzion = $0 . " version " . $verzion;
	my $nnow = localtime;
	my $gen_hdr_str = "/* TIDYCAT_BEGIN_CODEGEN \n\n";
	$gen_hdr_str .= "   WARNING: DO NOT MODIFY THE FOLLOWING SECTION: \n" .
		"   Generated by " . $verzion . "\n" . 
		"   on " . $nnow . "\n*/\n\n";

	my ($vol0, $dirs0, $base0) = File::Spec->splitpath($0);

	my $tmp_indexing = File::Spec->catfile($glob_tmpdir, "indexing.h");
	my $fil_indexing = File::Spec->catfile($dirs0, "indexing.h");
	my $buf_indexing;
	my @arr_indexing;

	my $tmp_pg_type = File::Spec->catfile($glob_tmpdir, "pg_type.h");
	my $fil_pg_type = File::Spec->catfile($dirs0, "pg_type.h");
	my $buf_pg_type;
	my @arr_pg_type;

	# dirs0 should be src/include/catalog
	my @srcdirs = File::Spec->splitdir($dirs0);
	push @srcdirs, "..";
	push @srcdirs, "..";

	# src/backend/catalog
	my $tmp_catalogc = File::Spec->catfile($glob_tmpdir, "catalog.c");
	my $fil_catalogc = File::Spec->catfile(
		File::Spec->catdir(@srcdirs, "backend", "catalog"), "catalog.c");
	my $buf_catalogc;
	my @arr_catalogc;
	my @arr_catalogc_tab;
	my @arr_catalogc_ind;

	# src/backend/bootstrap
	my $tmp_bootparse = File::Spec->catfile($glob_tmpdir, "bootparse.y");
	my $fil_bootparse = File::Spec->catfile(
		File::Spec->catdir(@srcdirs, "backend", "bootstrap"), "bootparse.y");
	my $buf_bootparse;
	my @arr_bootparse;

	my %alltabs;

	for my $filnam (@ARGV)
	{
		%alltabs = ();

		my ($vol, $dirs, $basename) = File::Spec->splitpath($filnam);

		my $elist = parsefile($filnam, 1); # file has def and codegen sections

		my $buffil = bufferfile($filnam);

		# do it in reverse order to strip out line numbering correctly
		for my $entry (reverse (@{$elist}))
		{
			my $bigstr = $entry->{def};
			
#	print $bigstr, "\n";

			parsetabdef(\%alltabs, $bigstr);

#	print Data::Dumper->Dump([%alltabs]);


#			print formattabs(\%alltabs);

			my $gentext = [];

			push @{$gentext}, split(/(\n)/, $gen_hdr_str);

			push @{$gentext}, split(/(\n)/, formattabs(\%alltabs));

			push @{$gentext}, "\n\n/* TIDYCAT_END_CODEGEN */\n";

			if (exists($entry->{begin_codegen}))
			{
				splice (@{$buffil}, 
						($entry->{begin_codegen}-1),
						($entry->{end_codegen} - $entry->{begin_codegen}) + 1,
						@{$gentext});
			}
			else
			{

				splice (@{$buffil}, ($entry->{enddef}),
						0,
						@{$gentext});
			}
		} # end for entry

		push @arr_indexing, formatindexes(\%alltabs);
		
		push @arr_pg_type, formattypes(\%alltabs);

		my $tab_ind = fix_issharedrelation_function(\%alltabs);

		push @arr_catalogc, $tab_ind;

		if (defined($tab_ind) && scalar(@{$tab_ind}))
		{
			push @arr_catalogc_tab, $tab_ind->[0]
				if (length($tab_ind->[0]));
			push @arr_catalogc_ind, $tab_ind->[1]
				if ((1 < scalar(@{$tab_ind})) && (length($tab_ind->[1])));
		}

		push @arr_bootparse, fix_bootparse(\%alltabs);

		# write the modified file to the temp directory
		open my $file_out, "> " . File::Spec->catfile($glob_tmpdir, $basename)
			or die "Cannot open $basename for write in $glob_tmpdir";

		# write the modified file to the tmpdir
		print $file_out join("", @{$buffil});

		close $file_out;

	} # end for filnam

	fixup_generated_code ("indexing.h", $fil_indexing, $tmp_indexing,
		\@arr_indexing, $verzion, $nnow)
		if (scalar(@arr_indexing));

#	print Data::Dumper->Dump(\@arr_catalogc_tab), "\n";

	# catalog.c IsSharedRelation table entries
	fixup_generated_code ("catalog.c tab", $fil_catalogc, $tmp_catalogc,
		\@arr_catalogc_tab, $verzion, $nnow)
		if (scalar(@arr_catalogc_tab));

	# catalog.c IsSharedRelation index entries
	fixup_generated_code ("catalog.c ind", $fil_catalogc, $tmp_catalogc,
		\@arr_catalogc_ind, $verzion, $nnow)
		if (scalar(@arr_catalogc_ind));

	# bootparse.y static type entries
	fixup_generated_code ("bootparse.y", $fil_bootparse, $tmp_bootparse,
		\@arr_bootparse, $verzion, $nnow)
		if (scalar(@arr_bootparse));


}

exit();
