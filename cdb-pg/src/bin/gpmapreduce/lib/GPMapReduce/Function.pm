package GPMapReduce::Function;

use 5.00503;
use strict;
use warnings;
use Safe;
use B::Deparse;
use GPMapReduce::Object;
use GPMapReduce::Utils;

require Exporter;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS @LANGUAGES);
@ISA = qw(Exporter GPMapReduce::Object);

# This allows declaration use GPMapReduce::Function ':all';
%EXPORT_TAGS = ( 'all' => [ qw() ] );
@EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
@EXPORT    = qw();
$VERSION   = '0.01';

# Supported languages and their SQL names
#   - keep the keys lowercase for easy lookup
my %LANGUAGES = ( c      => 'C', 
				  r      => 'plr',
				  perl   => 'plperl', 
				  python => 'plpythonu',
				  pgsq   => 'plpgsql',
				  sql    => 'sql');
my $LANGUAGES = join('|', keys %LANGUAGES);

sub new {
	my $class = shift;
	my $self  = new GPMapReduce::Object(@_);

	$self->Parameter('Input', Required => 1, Kind => 'ARRAY');
	$self->Parameter('Output', Required => 1, Kind => 'ARRAY');
	$self->Parameter('Language', Default => 'perl');
	$self->Parameter('Mode', Default => 'SINGLE');
	$self->Parameter('Optimize');
	$self->Parameter('Body');
	$self->Parameter('Function');

	return bless $self, $class;
}

sub CheckInput($$) 
{
	my ($self, $objects) = (shift, shift);
	my ($i, $param, $name, $type);
	
	# Call superclass
	GPMapReduce::Object::CheckInput($self, $objects);

	# Mode can only be SINGLE or MULTI, and defines whether the
	# function should return a set.
	$self->{Mode} = uc($self->{Mode});
	if (not ($self->{Mode} =~ m/^(SINGLE|MULTI)$/)) {
		Error("Unknown MODE: $self->{Name}: {MODE: $self->{Mode}} ");
	} 
#
#elsif ($self->{Mode} eq 'MULTI' and $self->{Language} ne 'C') {
#		Error("$self->{Name}: {MODE: MULTI} not supported for $self->{Language} functions");
#	}
	

	# Function specific extensions
	$self->SetFunction( Body => $self->{Body},
						Language => $self->{Language},
						Function => $self->{Function} );
	
	# Break input into name/type pairs
	$name = [];
	$type = [];
	foreach $param (@{$self->{Input}}) {
		my @p = split(' ', $param);
		if ($#p == 0) {
			push @$name, $p[0];
			push @$type, 'text';
		} elsif ($#p == 1) {
			push @$name, $p[0];
			push @$type, $p[1];
		} else {
			Error("Bad Input Parameter: $param");
		}
	}
	$self->{Input_name} = $name;
	$self->{Input_type} = $type;

	# Break output into name/type pairs
	$name = [];
	$type = [];
	foreach $param (@{$self->{Output}}) {
		my @p = split(' ', $param);
		if ($#p == 0) {
			push @$name, $p[0];
			push @$type, 'text';
		} elsif ($#p == 1) {
			push @$name, $p[0];
			push @$type, $p[1];
		} else {
			Error("Bad Output Parameter: $param");
		}
	}
	$self->{Output_name} = $name;
	$self->{Output_type} = $type;

	# reconstruct input/output with default types
	for ($i = 0; $i <= $#{$self->{Input}}; $i++) {
		${$self->{Input}}[$i] = 
			"${$self->{Input_name}}[$i] ${$self->{Input_type}}[$i]";
	}
	for ($i = 0; $i <= $#{$self->{Output}}; $i++) {
		${$self->{Output}}[$i] = 
			"${$self->{Output_name}}[$i] ${$self->{Output_type}}[$i]";
	}
}


#
#  Fixme - age this out
#
sub Summary 
{
    my $self = shift;

    TypeCheck($self, "GPMapReduce::Function", "\$self", 0);
    if ($#_ >= 0) {
		my $func = (caller(0))[3];
		Error("Too many parameters");
	}
    
    print STDERR "Function {\n" .
	"  name     => $self->{Name},\n" .
	"  in       => [" . join(', ', @{$self->{Input}}) . "],\n" .
	"  in_name  => [" . join(', ', @{$self->{Input_name}}) . "],\n" .
	"  in_type  => [" . join(', ', @{$self->{Input_type}}) . "],\n" .
	"  out      => [" . join(', ', @{$self->{Output}}) . "],\n" .
	"  out_name => [" . join(', ', @{$self->{Output_name}}) . "],\n" .
	"  out_type => [" . join(', ', @{$self->{Output_type}}) . "],\n" .
	"  lang     => $self->{Lang}\n" .
	"  body     => $self->{Body}\n" .
	"}\n";

    return undef;
}


sub SetFunction 
{
    my $self     = shift;
    my %param    = Params([qw(?Body ?Language ?Function)], @_);
    my $body     = $param{"Body"};
    my $function = $param{"Function"};
    my $language = $param{"Language"};

    TypeCheck($self, "GPMapReduce::Function", "\$self", 0);
    TypeCheck($language, "SCALAR", "Language", 1);
    TypeCheck($function, "CODE", "Function", 1);

    if (!$body and !$function) {
		if ($language) { 
			Error("Not enough Parameters"); 
		}
		$self->{"Body"} = undef;
		$self->{"Lang"} = undef;
		return;
    }

    TypeCheck($body, "SCALAR", "Body", 1);

    if (defined($function) and (defined($body) or defined($language))) {
		Error("Incompatable Parameters - Function + Body/Language");
    }

    # Default language is perl
    $language = "perl" unless($language);

    # Check if the language is supported
    if ($language and !($language =~ /^$LANGUAGES$/i)) {
		Error("Bad Parameter ( Language => $language )");
    }

    # For perl language functions we do extra validation by
    #   1) create the function 
    #   2) deparse the function
    #
    # We'll validate other languages after we create the SQL
    # functions.
    if ($body and lc($language) eq "perl") { 
		$function = eval("package main; sub {$body}"); 
    }

    if ($function)
    {
		my $dp = B::Deparse->new("-P", "-sC");
		$dp->ambient_pragmas(strict => 'all', warnings => 'all');
		$body = $dp->coderef2text($function);

	# If the function is inside of a package the unparse will have
	# the package name, it's better if we remove it.
		$body =~ s/\n *package [^;]+;//gms;
		$body =~ s/\n *no strict [^;]+;//gms;
    }
    
    $self->{"Body"} = $body;
    $self->{"Lang"} = $language;
}

sub SQLDrop
{
    my $self = shift;
	my $drop = "DROP FUNCTION IF EXISTS $self->{Name}(";
	$drop .= join(', ', @{$self->{Input_type}}); 
	$drop .= ") CASCADE;\n";

	# If we created a return type, drop that too
	if ($#{$self->{Output}} > 0) {
		$drop .= "DROP TYPE IF EXISTS $self->{Name}_Return CASCADE;\n";
	}
    return $drop;
}

sub SQLCreate
{
    my $self = shift;
    my $in = $self->{Input};
    my $out = $self->{Output};
    my $lang = $LANGUAGES{lc($self->{Lang})};
    my $opt = defined($self->{Optimize}) ? " $self->{Optimize}" : "";
	my $create = '';
	my $outtype;

    # Identity functions can be skipped
    return undef unless($self->{Body});

	# If we are returning multiple columns then we need to create a Type
	# for the return value.  (While it is possible to create functions
	# using anonymous records there are too many things that don't work
	# if you do.)
	if ($#{$self->{Output}} == 0) {
		$outtype = ${$self->{Output_type}}[0];
	} else {
		$outtype = "$self->{Name}_Return";
		$create .= "CREATE TYPE $outtype as (";
		$create .= join(', ', @{$self->{Output}});
		$create .= ");\n";
	}
	$create .= "CREATE FUNCTION $self->{Name}(";
	$create .= join(', ', @{$self->{Input}});
	if ($self->{Mode} eq 'MULTI') {
		$create .= ") RETURNS SETOF $outtype AS \$\$\n";
	} else {
		$create .= ") RETURNS $outtype AS \$\$\n";
	}
	$create .= "$self->{Body}\n\$\$ LANGUAGE $lang$opt;\n";

	return $create;
}


################################################################

=head1 SEE ALSO

GPMapReduce::Mapper, GPMapReduce::Transition,
GPMapReduce::Combiner, GPMapReduce::Finalizer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__
