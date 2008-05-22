package GPMapReduce::Function;

use 5.00503;
use strict;
use warnings;
use Safe;
use B::Deparse;
use GPMapReduce::Utils;

require Exporter;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS @LANGUAGES);
@ISA = qw(Exporter);

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
	Error("GPMapReduceFunction is an abstract class");
}

sub Initialize {
    my $self = shift;
    my %param = Params([qw(?Name  ?Input    ?Output  ?Language
			               ?Body  ?Optimize ?Function)], @_);

    TypeCheck($self, "GPMapReduce::Function", "\$self", 0);

    # Maybe consider allowing anonymous functions later?
    TypeCheck($param{Name}, "SCALAR", "Name", 0);
    TypeCheck($param{Input}, "SCALAR", "Input", 0);
    TypeCheck($param{Output}, "SCALAR", "Output", 0);
    TypeCheck($param{Optimize}, "SCALAR", "Optimize", 1);
    $self->SetFunction( Body => $param{Body},
					    Language => $param{Language},
					    Function => $param{Function});    

    # FIXME: typecheck Input and Output


    # For better or worse we allow anonymous functions
    $self->{Name}     = $param{Name};
    $self->{Input}    = $param{Input};
    $self->{Output}   = $param{Output};
    $self->{Optimize} = $param{Optimize};

    return undef;
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
	"  name => $self->{Name},\n" .
	"  in   => $self->{Input},\n" .
	"  out  => $self->{Output},\n" .
	"  lang => $self->{Lang}\n" .
	"  body => $self->{Body}\n" .
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

sub GenerateSQL_Drop
{
    my $self = shift;
    return "DROP FUNCTION $self->{Name}($self->{Input}) CASCADE;\n";
}

sub GenerateSQL_Create
{
    my $self = shift;
    my $in = $self->{Input};
    my $out = $self->{Output};
    my $lang = $LANGUAGES{lc($self->{Lang})};
    my $opt = defined($self->{Optimize}) ? " $self->{Optimize}" : "";

    # Identity functions can be skipped
    return undef unless($self->{Body});

    # FIXME - this is only apropriate for non-set returning functions
    $in =~ s/, ?/, IN /g;
    $out =~ s/, ?/, OUT /g;

    return 
	"CREATE FUNCTION $self->{Name}(IN $in, OUT $out) AS \$\$\n" .
	"$self->{Body}\n\$\$ LANGUAGE $lang$opt;\n";
}


################################################################

=head1 SEE ALSO

GPMapReduce::Mapper, GPMapReduce::Transition,
GPMapReduce::Combiner, GPMapReduce::Finalizer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: cwelton@greenplum.com

=cut

################################################################

1;
__END__
