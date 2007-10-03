#!/usr/bin/perl
# Copyright Greenplum 2007
# Author: Gavin Sherry
# Tool to generate SQL reflecting differences between BKI files

use Pod::Usage;
use Data::Dumper;

=head1 NAME

B<genbkidiff.pl> Generate differences, as SQL, between PostgreSQL BKI files

=head1 SYNOPSIS

B<genbkidiff.pl> <from> <to> <outfile>

=head1 AUTHORS

Gavin Sherry

=cut

# We want to step through the two files finding lines which differ as we
# go. Determining how and where a line differs isn't trivial because we
# need to know a) if the line is a new entry or an update 2) if the latter
# which fields have been updated. There's no point using a longest common
# sistring algorithm because we care about fields. Luckily the data inserted
# is space delimited AND all tables have a primary key, so we can be sure
# that we do not UPDATE where we should ADD a new entry.

sub outputsql
{
	my $name;

	if ($is_upgrade)
	{
		$name = "upgrade_cmds";
	}
	else
	{
		$name = "downgrade_cmds";
	}

	print OUTFILE "/* 
 * WARNING!!! This is a system generated file. Use gpbkidiff to generate it
 * or bug Gavin Sherry.
 */

#ifndef GP" . uc($name) . "_H
#define GP" . uc($name) . "_H

const char *" . $name . "[] = {";

	print OUTFILE $typebuf;
	print OUTFILE $sqlbuf;
	print OUTFILE "	NULL};

#endif /* GP" . uc($name) . "_H */
";
}

sub printsql
{
	my $str = shift;
	$str =~ s/"/\\"/g;
	chomp $str;
	$str =  "	\"$str\",\n";
#	$str = $str . "\n";

	if ($str =~ m/pg_type/)
	{
		$typebuf = $typebuf . $str;
	}
	else
	{
		$sqlbuf = $sqlbuf . $str;
	}
}

sub mylog
{
	my $type = shift;
	my $msg = shift;

	print "$type: $msg";

	printsql $msg if $type eq SQL;
}

sub linegetarray 
{
	my $text = shift;

	@new = ();
	while($text =~ m{
		"([^\"\\]*(?:\\.[^\"\\]*)*)"[ ]?  # groups the phrase inside the quotes
		  | ([^ ]+)[ ]?
		  | [ ]
			}gx)
	{
		my $col = $+;
		chomp($col);
		push @new, $col;
	}
#	push(@new, undef) if substr($text,-1,1) eq ' '; 

	return(@new);
}

# args: line, file handle

sub process_line
{
	my $line = shift;
	local(*LINE) = shift;
	my $state;

	my $saveline = $line;

	if ($line =~ /^insert/)
	{
		my $oid = undef;
		if ($line =~ m/^insert OID = \d+.*/)
		{
			$oid = $line;
			$oid =~ s/^insert OID = (\d+).*/\1/;
			chomp $oid;
		}
		print "oid = $oid\n";
		$line =~ s/^insert[^(]*[(] (.*)[)]$/\1/; 
		chomp($line);
		my @larr = linegetarray($line);
		$state = "INSERT";

		return ($state, $saveline, $oid, @larr);
	}
	elsif ($line =~ /^create/)
	{
		my $table_name;

		$line =~ s/create (bootstrap|without_oids|shared_relation| )*//;
		$line =~ s/(\w+).*/\1/;
		$table_name = $line;
		chomp($table_name);

		#print "table name = "; print $table_name;
		# read until we get to the end of the creation section
		$line = <LINE>;
		die "error" unless $line == " (";
		my %cols;
		my @col_order;

		for(;;)
		{
			$line = <LINE>;

			$saveline = $line;

			last if $line =~ m/^ \)$/;

			$line =~ s/\s//g;
			$line =~ s/,$//;

			my @pair = split "=", $line;

			$cols{@pair[0]} = @pair[1];
			push @col_order, @pair[0];
		}

		my %ret = ("order" => [@col_order], "cols" => { %cols} );
		
#		$ret{"order"} = @col_order;
#		$ret{"cols"} = %cols;

#		print "dumping\n";
#		print Dumper %ret;
		return ("CREATE", $saveline, $table_name, \%ret);

	}
	elsif ($line =~ m/^declare/ || $line =~ m/^build/ ||
		   $line =~ m/^\s*$/ || $line =~ m/^\s*#.*$/ || 
		   $line =~ m/^open/)
	{
		$line = <LINE>;
		return undef unless $line;

		return (process_line($line, *LINE));
	}
	elsif ($line =~ m/^close/)
	{
		mylog(DEBUG, "returning CLOSE\n");
		return ("CLOSE", $saveline);
	}
	else
	{
		die "BUG, unknown line $saveline";
	}
}

# test if the primary keys in row1 and row2 are the same

sub pkey_equal
{
	my $table = shift;
	my $row1 = shift;
	my $row2 = shift;
	my $pkeys = shift;

#	print Dumper(%$pkeys);
	mylog("DEBUG", "table = $table, $$pkeys{$table}, '%$pkeys'\n");
	return 0 unless exists($$pkeys{$table});

	mylog("DEBUG", "found key\n");

	my $equal = 1;

	my @cols = $$pkeys{$table};

#	print Dumper(@cols);
#	print Dumper(%$row1);
#	print Dumper(%$row2);

	my $col;
	for $col (@{$pkeys->{$table}})
	{
		mylog("DEBUG", Dumper($col));
		mylog("DEBUG", "col = $col, 1: $$row1{$col}, 2: $$row2{$col}\n");
		return 0 unless exists($$row1{$col}) and exists($$row2{$col});

		if ($$row1{$col} ne $$row2{$col})
		{
			mylog("DEBUG", "found '$$row1{$col}' ne '$$row2{$col}'\n");
			$equal = 0;
			last;
		}
	}
	return $equal;
}

# need types too :(
sub sqlify
{
	my $colname = shift;
	my $row = shift;
	my $cols = shift;

#	print Dumper($row);
#	mylog(DEBUG, "colname = $colname, value = $row->{$colname}\n");
	
	my $a = $row->{$colname};

	if ($a eq "_null_")
	{
		return "NULL";
	}
	else
	{
		$a =~ s/'/''/g;
		return "'" . $a . "'::$cols->{$colname}";
	}

}

# given $table, %row, %oldrow, %cols, @colorder, @oldcolorder, $oid, $oldoid, 
#	%pkey 
# make an update statement. We are careful to update on the rows which 
# have changed.
sub rowmakeupdate
{
	my $table = shift;
	my $row = shift;
	my $oldrow = shift;
	my $cols = shift;
	my $colorder = shift;
	my $oldcolorder = shift;
	my $oid = shift;
	my $oldoid = shift;
	my $pkey = shift;

#	print "\nupdate\n";
#	print Dumper %$cols;
	my $found = 0;

	# are the non-system attributes the same? If so, OID only change 

	my $sql = "UPDATE pg_catalog.\"$table\" SET ";

	my $bar;
	my @foo;
	
	foreach $bar (keys %$row)
	{
		# update all fields anyway, since we want to duplicate rows even
		# if only the OID changed
		push @foo, ("\"$bar\" = " . sqlify($bar, \%$row, \%$cols)); # if ($row->{$bar} ne $oldrow->{$bar});
		#mylog(DEBUG, "testing $row->{$bar} ne $oldrow->{$bar}\n");
	}

	$found = scalar(@foo);

	if ($found)
	{
		$sql = $sql . (join ", ", @foo) . " WHERE ";

		die "shouldn't get here - $table\n" unless exists $pkey->{$table};

		my @attrs = @{$pkey->{$table}};

		@foo = ();
		my $bar;
	
		foreach $bar (@attrs)
		{
			push @foo, "$bar = " . sqlify($bar, \%$oldrow, \%$cols);
		}
		$sql = $sql . (join " AND ", @foo);

		# add returning statement
		if (defined($oid))
		{
#			$sql = $sql . " returning oid as oldoid, '$oid'::oid as newoid, " .
#				"ctid, tableoid";
			$sql = $sql . " returning gp_oid_update(oid, '$oid'::oid, ctid, 'pg_catalog.$table'::regclass::oid)";
		}
		$sql = $sql . ";";
	}
	else
	{
		# the difference must be OIDs!
		die "BUG\n";

#		 if ($oid eq $oldoid);
#		mylog(SQL, "insert into __gpupgrademap values($table, $oldoid, $oid);\n");
	}
	return $sql;
}

# given $table, %row, %cols, @colorder, $oid make an insert statement
sub rowmakeinsert
{
	my $table = shift;
	my $oid = shift;
	my $row = shift;
	my $cols = shift;
	my $colorder = shift;

	my $sql = "INSERT INTO pg_catalog.\"$table\" ";

	$sql = $sql . "(" . (join ", ", keys(%$row)) . ") VALUES ";
	$sql = $sql . "(" . (join ", ", 
			map { $_ = sqlify($_, \%$row, \%$cols); } keys %$row) . ") ";

	mylog(DEBUG, "222OID = $oid\n");
#	$sql = $sql . " returning oid as oldoid, " .
#				"'$oid'::oid as newoid, ctid, tableoid" if defined($oid);
	$sql = $sql . " returning gp_oid_update(oid, '$oid'::oid, ctid, tableoid);"
		if defined($oid);

	$sql = $sql . ";";
	return $sql;
}

# given $table, $oid, %row, %cols, @colorder, pkey make a delete statement
sub rowmakedelete
{
	my $table = shift;
	my $oid = shift;
	my $row = shift;
	my $cols = shift;
	my $colorder = shift;
	my $pkey = shift;

	my $sql = "DELETE FROM pg_catalog.\"$table\" WHERE ";

	die "shouldn't get here - $table\n" unless exists $pkey->{$table};

	my @attrs = @{$pkey->{$table}};

	@foo = ();
	my $bar;
	
	foreach $bar (@attrs)
	{
		push @foo, "$bar = " . sqlify($bar, \%$row, \%$cols);
	}
	$sql = $sql . (join " AND ", @foo);

	return $sql;
}

pod2usage() unless scalar(@ARGV) >= 3;

my $tmp = @ARGV[0];

our $is_upgrade = 1;

mylog(DEBUG, "tmp = $tmp\n");
if ($tmp eq "-D")
{
	$is_upgrade = 0;
} 

our $outfile = pop @ARGV;
my $f2 = pop @ARGV;
my $f1 = pop @ARGV;

# buffer sql
our $typebuf;
our $sqlbuf;

mylog(DEBUG, "out = $outfile, f2 = $f2, f1 = $f1\n");

for my $fname ($f1, $f2)
{
	unless (-e $fname)
	{
		print STDERR "genbkidiff.pl: $fname: No such file or directory\n";
	}
}
exit(2)
	unless ((-e $f1) && (-e $f2));

$F1 = $f1;
$F2 = $f2;
open F1 or die "cannot open $F1\n";
open F2 or die "cannot open $F2\n";

unlink $outfile if (-e $outfile);

our $OUTFILE;

open(OUTFILE, ">$outfile") or die "cannot open $outfile\n";

# states
use enum qw(NEW_BOTH NEW_LEFT NEW_RIGHT);

my %pkeys = ("pg_proc" => ["proname", "proargtypes", "pronamespace"],
			 "pg_type" => ["typname", "typnamespace"],
			 "pg_attribute" => ["attrelid", "attnum"],
			 "pg_class" => ["relname", "relnamespace"],
			 "pg_autovacuum" => ["varelid"],
			 "pg_attrdef" => ["adrelid", "adnum"],
			 "pg_constraint" => [ ], # no unique index
			 "pg_inherits" => ["inhrelid", "inhseqno"],
			 "pg_index" => ["indexrelid"],
			 "pg_operator" => ["oprname", "oprleft", "oprright", "oprnamespace"],
			 "pg_opclass" => ["opcamid", "opcname", "opcnamespace"],
			 "pg_am" => ["amname"],
			 "pg_amop" => ["amopopr", "amopclaid"],
			 "pg_amproc" => ["amopclaid", "amprocsubtype", "amprocnum"],
			 "pg_language" => ["lanname"] ,
			 "pg_largeobject" => ["loid", "pageno"],
			 "pg_aggregate" => ["aggfnoid"],
			 "pg_statistic" => ["starelid", "staattnum"],
			 "pg_rewrite" => ["ev_class", "rulename"],
			 "pg_trigger" => ["tgrelid", "tgname"],
			 "pg_listener" => [ ],
			 "pg_description" => ["objoid", "classoid", "objsubid"],
			 "pg_cast" => [ "castsource", "casttarget" ],
			 "pg_namespace" => [ "nspname" ],
			 "pg_conversion" => [ "conname", "connamespace" ],
			 "pg_depend" => [ ], # no unique index
			 "pg_database" => [ "datname"],
			 "pg_tablespace" => [ "spcname" ],
			 "pg_pltemplate" => [ "tmplname" ],
			 "pg_authid" => [ "rolname"],
			 "pg_auth_members" => [ "member", "roleid" ],
			 "pg_shdepend" => [ ], # no unique
			 "pg_shdescription" => [ "objoid", "classoid" ],
			 "pg_resqueue" => [ "rsqname"],
			 "gp_configuration" => [ "dbid"],
			 "gp_id" => [ ], # no unique
			 "gp_distribution_policy" => [ "localoid" ],
			 "gp_version_at_initdb" => [ ], # no unique
			 "pg_window" => [ "winfnoid"],
			 "pg_exttable" => [ "reloid"]);

my $cur_table1;
my %cur_cols1;
my @cur_colorder1;
my $state1;
my $oid1;

my $cur_table2;
my %cur_cols2;
my @cur_colorder2;
my $state2;
my $oid2;

my $READ_STATE = NEW_BOTH;

my $line1;
my $line2;

my %row1;
my %row2;
for (;;)
{
	if ($READ_STATE == NEW_BOTH || $READ_STATE == NEW_LEFT)
	{
		$line1 = <F1>;
		mylog("DEBUG", "read line1, $. line1 = $line1\n");
	}

	if ($READ_STATE == NEW_BOTH || $READ_STATE == NEW_RIGHT)
	{
		$line2 = <F2>;
		mylog("DEBUG", "read line2, $. line2 = $line2\n");
	}

	my @ret;

	last unless $line1 or $line2;

	if ($READ_STATE == NEW_BOTH || $READ_STATE == NEW_LEFT)
	{
		# process line 1
		mylog("DEBUG", "line 1 processing\n");
		@ret = process_line($line1, *F1);

		$state1 = shift(@ret);
		$line1 = shift(@ret);
	
		if ($state1 =~ m/CREATE/)
		{
			print "ret\n";
			$cur_table1 = shift(@ret);
			my %ret = %{shift(@ret)};

			%cur_cols1 = undef;
			@cur_colorder1 = undef;
			%row1 = ();

#			print "dump2 ", Dumper %ret;
			@cur_colorder1 = @{$ret{"order"}};
			%cur_cols1 = %{$ret{"cols"}};

			print "fo\n", Dumper @cur_colorder1;
			print "ba\n", Dumper %cur_cols1;
	
			mylog("DEBUG", "cur table = $cur_table1, cols %{$cur_cols1}, order = @cur_colorder1\n");
		}
		elsif ($state1 =~ m/INSERT/)
		{
			$oid1 = shift(@ret);
			my @data = @ret;
			mylog("DEBUG", "insert, cur table = $cur_table1, data = \"@data\"\n");
	
			my @_cols = @cur_colorder1;
	
			my $count = 0;
	
			if (scalar(@data) != scalar(@_cols))
			{
				mylog(DEBUG, "BUG, must be equal\n");
				mylog(DEBUG, "1: " . scalar(@data) . " " . scalar(@_cols) . "\n");
				mylog(DEBUG, "'@data' | '@_cols'\n");
				die;
			}
	
			for (;;)
			{
				$count++;
				my $col = shift(@_cols);
				last unless $col;
	
				my $v = shift(@data);
	
	#			print "length = "; print scalar(@data);
				if (!defined($v))
				{
					print "BUG! col count ne to val count, $count, $col, $v, @data\n";
					die;
				}
				$row1{$col} = $v;
			}
		}
	}

	if ($READ_STATE == NEW_BOTH || $READ_STATE == NEW_RIGHT)
	{
		mylog(DEBUG, "line 2 processing\n");
	
		@ret = process_line($line2, *F2);
	
		$state2 = shift(@ret);
		$line2 = shift(@ret);
	
		if ($state2 =~ m/CREATE/)
		{
			print "ret\n";
			$cur_table2 = shift(@ret);
			my %ret = %{shift(@ret)};

			@cur_colorder2 = undef;
			%cur_cols2 = undef;
#			print "dump2 ", Dumper %ret;
			@cur_colorder2 = @{$ret{"order"}};
			%cur_cols2 = %{$ret{"cols"}};
			%row2 = ();

			print "fo\n", Dumper @cur_colorder2;
			print "ba\n", Dumper %cur_cols2;
	
			mylog("DEBUG", "cur table = $cur_table2, cols %{$cur_cols2}, order = @cur_colorder2\n");
		}
		elsif ($state2 =~ m/INSERT/)
		{
			$oid2 = shift(@ret);
			my @data = @ret;
			mylog(DEBUG, "insert, cur table = $cur_table2, oid = $oid2, data = @data\n");
	
			my @_cols = @cur_colorder2;
			print @_cols;
	
			if (scalar(@data) != scalar(@_cols))
			{
				mylog(DEBUG, "BUG, must be equal\n");
				mylog(DEBUG, "1: ", scalar(@data), "\n" , scalar(@_cols), "\n");
				die;
			}
	
			for (;;)
			{
				$count++;
				my $col = shift(@_cols);
				last unless $col;
	
				my $v = shift(@data);
	
	#			print "length = "; print scalar(@data);
				if (!defined($v))
				{
					mylog(BUG," col count ne to val count, $count, $col, $v, @data\n");
					die;
				}
				$row2{$col} = $v;
			}
		}
	}

	if ($state1 =~ m/INSERT/ and $state1 eq $state2)
	{
		# same table?

		if ($cur_table1 eq "gp_version_at_initdb")
		{
			# update state ?
			next;
		}

		if ($cur_table1 eq $cur_table2)
		{
			if ($line1 eq $line2)
			{
				$READ_STATE = NEW_BOTH;
				mylog(DEBUG, "strings are equal: \n\"$line1\"\n\"$line2\"");
				mylog(DEBUG, "setting read state to NEW_BOTH\n");
			}
			else
			{
				# If it's the same primary key, UPDATE otherwise INSERT
				my $test = pkey_equal($cur_table1, \%row1, \%row2, \%pkeys);

				mylog(DEBUG, "test = $test\n");

				# XXX: temporary hack for now to deal with Feng's AVG
				# changes
				my %hack = ("float8_avg" => 1, "numeric_avg" => 1,
							"int2_avg_accum" => 1, "int4_avg_accum" => 1,
							"int8_avg" => 1, "int8_avg_amalg"); 
				if ($test or 
					($cur_table1 eq "pg_proc" and exists($hack{$row2{"proname"}})))
				{
					print "line1 = \"$line1\"\nline2 = \"$line2\"\n";
					# issue UPDATE
					my $sql = rowmakeupdate($cur_table2, \%row2, \%row1,
											\%cur_cols2, \@cur_colorder2,
											\@cur_colorder1, $oid2, $oid1,
											\%pkeys);
					$READ_STATE = NEW_BOTH;
					mylog(SQL, "$sql\n");
				}
				else
				{
					# if we're doing an upgrade, it's an insert, if not,
					# it's a delete
	
					if ($is_upgrade)
					{
						mylog(DEBUG, "oid = $oid2\n");
						mylog(SQL, rowmakeinsert($cur_table2,$oid2, 
												 \%row2, \%cur_cols2) . "\n");
						# state = GET_NEW_RIGHT
						$READ_STATE = NEW_RIGHT;
						mylog(DEBUG, "setting readstate to NEW_RIGHT\n");
					}
					else
					{
						mylog(DEBUG, "oid = $oid1\n");
						mylog(SQL, rowmakedelete($cur_table1, $oid1, 
												 \%row1, \%cur_cols1, 
												  \@cur_colorder1, \%pkeys) . 
												"\n");
						$READ_STATE = NEW_LEFT;
						mylog(DEBUG, "setting readstate to NEW_LEFT\n");

					}
				}
			}
		}
		else
		{
			# XXX: what now?
			die "shouldn't get here - table1 = $cur_table1, $cur_table2\n";
		}
	}
	elsif ($state1 =~ m/CLOSE/ and $state2 =~ m/INSERT/)
	{
		mylog(DEBUG, "state1 = close, state2 = insert\n");
		die "state mix up: cur_table1 = $cur_table1, cur_table2 = $cur_table2\n"
			if $cur_table1 ne $cur_table2;

		mylog(SQL, rowmakeinsert($cur_table2, $oid2, \%row2, \%cur_cols2, $oid2) . "\n");

		$READ_STATE = NEW_RIGHT;
	}
	elsif ($state1 =~ m/CREATE/ and $state2 =~ m/CREATE/)
	{
		# c'est bon, next
	}
	elsif ($state1 =~ m/CLOSE/ and $state2 eq $state1)
	{
		die "state mismatch: $cur_table1 ne $cur_table2\n"
			if ($cur_table1 ne $cur_table2);

		$READ_STATE = NEW_BOTH;
	}
	elsif ($state1 =~ m/INSERT/ and $state2 =~ m/CLOSE/)
	{
		mylog(DEBUG, "state1 = insert, state2 = close\n");
		die "state mix up: cur_table1 = $cur_table1, cur_table2 = $cur_table2\n"
			if $cur_table1 ne $cur_table2;

		mylog(SQL, rowmakedelete($cur_table2, $oid1, \%row1, \%cur_cols1, \@cur_colorder1, \%pkeys ) . "\n");

		$READ_STATE = NEW_LEFT;

	}
	elsif (!defined($state1) and !defined($state2))
	{
		mylog(DEBUG, "END\n");
	}
	else
	{
		die "unprocessed state: $state1, $state2\n";
	}
}

outputsql();

exit(0);
