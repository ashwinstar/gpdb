package GPMapReduce::Parser;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Parser - Translates a YAML parse tree into a GPMapReduce::Document.

=head1 SYNOPSIS

    use YAML::Tiny;
    use GPMapReduce::Parser;

    my $yaml = new YAML::Tiny;
    my $docs = $yaml->read($file);
    foreach my $ydoc (@$docs) {
	my $doc = $parser->Parse($ydoc, @param);
	print $doc->GenerateSQL() . "\n";
    }


=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::*

=cut

################################################################

BEGIN 
{
    require 5.00503;
    use Carp;
    use GPMapReduce::Document;
    use GPMapReduce::Mapper;
	use GPMapReduce::Utils;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}


################################################################
#
# Some hashes describing acceptable attributes and the values they
# can take
#
my $url = '\S+';
my $name = '\w+';
my $fname = '.*';

my $RUN = 
{
 "SOURCE"       => "$url",
 "TARGET"       => "$url",
 "MAPPER"       => "$name",
 "REDUCER"      => "$name",
 "MODE"         => '\w+'           #   'append|replace'
};

my $TASK = 
{"NAME"        => "$name", 
 "SOURCE"      => "$url",
 "MAPPER"      => "$name", 
 "REDUCER"     => "$name", 
 "OUTPUT"      => "$name"
};

my $AGG = 
{"NAME"        => "$fname",
 "INITIALIZE"  => '.*', 
 "TRANSITION"  => "$name", 
 "COMBINE"     => "$name",
 "FINALIZE"    => "$name"
};

my $FUNC = 
{"NAME"        => "$fname",
 "LANGUAGE"    => '\w+', 
 "FUNCTION"    => '.*',
 "OPTIMIZE"    => '.*',
 "PARAMETERS"  => [],
 "RETURNS"     => [],
 "MODE"         => '\w+'           #   'SINGLE|MULTI'
};

my $SPLIT = 
{"NAME"        => "$fname",
 "LANGUAGE"    => '\w+',
 "FUNCTION"    => '.*',
};

my $INPUTS = 
{"NAME"        => "$name",
 "FILE"        => [], 
 "GPFDIST"     => [], 
 "TABLE"       => "$url",
 "QUERY"       => ".*", 
# "EXEC"        => ".*",
 "COLUMNS"     => [], 
 "FORMAT"      => ".*", 
 "DELIMITER"   => ".*",
 "SPLITTER"    => "$name"
};

my $EXEC = 
{
 "RUN"         => $RUN 
};

my $DEFINES = 
{
 "INPUT"       => $INPUTS, 
 "SPLIT"       => $SPLIT,
 "MAP"         => $FUNC,
 "TRANSITION"  => $FUNC,
 "COMBINER"    => $FUNC,
 "FINALIZER"   => $FUNC,
 "REDUCER"     => $AGG, 
 "TASK"        => $TASK
};

my $TOP = 
{"DATABASE"    => "$name",
 "USER"        => "$name",
 "HOST"        => "$name",
 "PORT"        => "\\d+",
 "VERSION"     => ".*",
 "DEFINE"      => $DEFINES,
 "EXECUTE"     => $EXEC
};

my $DOCUMENT = {"TOP" => $TOP};

sub new() {
    my $class = shift;

    return bless {
		"PARSE"    => undef,
		"DOCUMENT" => undef,
		"ERROR"    => undef,
    }, $class;
}

sub Parse($$@) 
{
    my $self = shift;
    my $ydoc = shift;
    my @param = @_;

    $self->{"ERROR"} = undef;
    $self->{"PARSE"} = $ydoc;
    $self->{"DOCUMENT"} = GPMapReduce::Document->new(@param);

    if (ref($ydoc) ne "HASH") {
		Error("Parse Error: Badly formed document\n");
	}
    if (!defined($ydoc->{"VERSION"})) {
		Error("Parse Error: required VERSION field is missing\n");
	}

	my $version = $ydoc->{"VERSION"};
	if ($version ne "1.0.0.1") {
		Error("Parse Error: Unknown VERSION: $version\n");
	}
	$self->_do_parse($DOCUMENT, "TOP", $ydoc);
	$self->_add_to_document("USER", $ydoc->{"USER"});
	$self->_add_to_document("DATABASE", $ydoc->{"DATABASE"});

    # for now we will die on error
    die $self->{"ERROR"} if (defined($self->{"ERROR"}));    

    return $self->{"DOCUMENT"};
}

sub _error($)
{
    my $self = shift;
    $self->{"ERROR"} = "Parse Error: " . shift;
}

#
# _do_parse - transform a YAML parse tree into a GPMapReduce::Document
#
sub _do_parse($$$$)
{
    my $self   = shift;       # me, myself, and I
    my $pstate = shift;       # the parse state 
    my $key    = uc(shift);   # the attribute we want to check in the state
    my $value  = shift;       # the value of the attribute

    # Die if the parse state doesn't have a match for the given key.
    #
    # Example 1: "COLUMNS" _is not_ a valid attribute for a MAPPER
    # Example 2: "COLUMNS" _is_ a valid attribute for an INPUT
    #
    return $self->_error("Mismatched tag ($key) at\n    $key:    $value\n")
		unless ($pstate->{$key});
    
    my $match = $pstate->{$key};
    
    #
    # Okay the attribute exists, but is the value valid?
    #
    if (!ref($value) && !ref($match))
    {
		# 1. $value and $match are both scalars
		#    In this case $match should contain a regular expression
		#    so we just need to check that they match.
		if ($key =~ /^($match)$/)
		{
			return 1;
		}
		$self->_error("Bad data value ($match) at\n    $key:    $value\n");
		return 0;
    }
    elsif (ref($value) && ref($match))
    {
		# 2. $value and $match are both indirect
		if (ref($value) eq "HASH")
		{
			#  a) $value is a hash
			#  We recurse on the key/value pairs for value
			
			foreach my $child (keys(%$value)) 
			{
				chomp($value->{$child});
				$self->_do_parse($match, $child, $value->{$child});
				return 0 if (defined $self->{"ERROR"});
			}
	    
			$self->_add_to_document($key, $value);
		}
		elsif (ref($value) eq "ARRAY")
		{
			#  b) $value is an array of hashes
			#     We recurse on the key/value pairs for each value in the array
			
			if (ref($match) eq "ARRAY") 
			{
				# accept it, assume contents are good for now
			}
			else
			{

				foreach my $elem (@$value) 
				{
					foreach my $child (keys(%$elem)) 
					{
						chomp($elem->{$child});
						$self->_do_parse($match, $child, $elem->{$child});
						return 0 if (defined $self->{"ERROR"});
					}
				}
			}
		}
		else 
		{
			# c) input contained some non-array,non-hash type
			return $self->_error("parsetree contains odd datatype " .
								 "$key->(" . ref($value) . ")");
		}
    }

	#   3. It is okay to get a scalar when we wanted an array 
	#      We'll just convert it to an array with one value later.
    elsif (ref($match) eq 'ARRAY')
    {

	}

	#   4. Run list can take a name (scalar) or job description (hash)
	elsif (not(ref($value)) and $match == $RUN)
	{
		$self->_add_to_document($key, $value);
	}

	#   5. Otherwise who knows what's gone wrong
	else
	{
		return $self->_error("scalar/pointer mismatch <$value, $match>\n");
    }
    
    return 1;
}

sub _add_to_document($$$) 
{
    my $self  = shift;
    my $key   = shift;
    my $value = shift;
    my $doc   = $self->{"DOCUMENT"};
    my $err;

    if ($key eq "TOP") 
    {
		
    }
    elsif ($key eq "USER") 
    {
		$err = $doc->SetUser($value);
    }
    elsif ($key eq "DATABASE")
    {
		$err = $doc->SetDatabase($value);
    }
    elsif ($key eq "HOST")
    {
		$err = $doc->SetHost($value);
    }
    elsif ($key eq "PORT")
    {
		$err = $doc->SetPort($value);
    }
    elsif ($key eq "INPUT") 
    {
		$doc->AddInput(Name      => $value->{NAME},
					   File      => $value->{FILE},
					   Table     => $value->{TABLE},
					   GPFDist   => $value->{GPFDIST},
					   Query     => $value->{QUERY},
					   Columns   => $value->{COLUMNS},
					   Format    => $value->{FORMAT},
					   Delimiter => $value->{DELIMITER});
    } 
    elsif ($key eq "MAP") 
    {
	    $doc->AddMapper(Name => $value->{NAME},
						Language => $value->{LANGUAGE},
						Body => $value->{FUNCTION},
						Optimize => $value->{OPTIMIZE},
						Mode => $value->{MODE},
                        Input => $value->{PARAMETERS},
                        Output => $value->{RETURNS});
    }
    elsif ($key eq "TRANSITION") 
    {
		$doc->AddTransition(Name => $value->{NAME},
							Language => $value->{LANGUAGE},
							Body => $value->{FUNCTION},
							Optimize => $value->{OPTIMIZE},
                            Input => $value->{PARAMETERS},
                            Output => $value->{RETURNS});
    }
    elsif ($key eq "COMBINER") 
    {
		$doc->AddCombiner(Name => $value->{NAME},
						  Language => $value->{LANGUAGE},
						  Body => $value->{FUNCTION},
						  Optimize => $value->{OPTIMIZE},
                          Input => $value->{PARAMETERS},
                          Output => $value->{RETURNS});
    }
    elsif ($key eq "FINALIZER")
    {
		$doc->AddFinalizer(Name => $value->{NAME},
						   Language => $value->{LANGUAGE},
						   Body => $value->{FUNCTION},
						   Optimize => $value->{OPTIMIZE},
                           Input => $value->{PARAMETERS},
                           Output => $value->{RETURNS});
    }
    elsif ($key eq "REDUCER") 
    {
		$doc->AddReducer(Name       => $value->{NAME},
                         Initialize => $value->{INITIALIZE},
						 Transition => $value->{TRANSITION},
						 Combiner   => $value->{COMBINE},
						 Finalizer  => $value->{FINALIZE});
    }
    elsif ($key eq "TASK") 
    {
        $doc->AddTask(Name    => $value->{NAME},
                      Source  => $value->{SOURCE},
					  Mapper  => $value->{MAPPER},
					  Reducer => $value->{REDUCER},
					  Output  => $value->{OUTPUT});
    }
    elsif ($key eq "RUN") 
    {
        if (!ref($value)) {
          $doc->AddRun(Source  => $value);
        } else {
          $doc->AddRun(Source  => $value->{SOURCE},
	  				   Mapper  => $value->{MAPPER},
					   Reducer => $value->{REDUCER},
                       Output  => $value->{TARGET});
        }
    }
    else 
    {
		die "$key is not a valid GPMapReduce::Document element";
    }

    if (defined($err)) {
		$self->_error("$err\n");
		die $self->{"ERROR"};
    }
}

1;

################################################################
# The GPMapReduce package itself is just a dummy package, use
# GPMapReduce::Document or GPMapReduce::Parser instead.
################################################################
package GPMapReduce;

=head1 NAME

GPMapReduce - The Greenplum Map/Reduce Driver package

=cut

require 5.00503;
use vars qw($VERSION @ISA );
$VERSION = '1.00';
@ISA = qw();

1;
__END__

