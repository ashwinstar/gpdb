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
{"SOURCE"       => "$url",
 "TARGET"       => "$url",
 "MAPPER"       => "$name",
 "REDUCER"      => "$name",
 "MODE"         => '\w+'           #   'append|replace'
};

my $JOB = 
{"NAME"        => "$name", 
 "SOURCE"      => "$url",
 "MAPPER"      => "$name", 
 "REDUCER"     => "$name", 
 "GROUPER"     => "$name", 
 "OPTIMIZE"    => '\w+'            #   'unordered'
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
 "BODY"        => '.*',
 "OPTIMIZE"    => '.*',
 "MODE"        => '\w+'
};

my $SPLIT = 
{"NAME"        => "$fname",
 "LANGUAGE"    => '\w+',
 "BODY"        => '.*',
};

my $INPUTS = 
{"NAME"        => "$name",
 "SOURCE"      => "$url", 
 "COLUMNS"     => ".*", 
 "FORMAT"      => ".*", 
 "DELIMITER"   => ".*",
 "SPLITTER"    => "$name"
};

my $EXEC = 
{"RUN"         => $RUN
};

my $DEFINES = 
{
 "INPUT"       => $INPUTS, 
 "SPLIT"        => $SPLIT,
 "MAP"         => $FUNC,
 "TRANSITION"  => $FUNC,
 "COMBINER"    => $FUNC,
 "FINALIZER"   => $FUNC,
 "REDUCER"     => $AGG, 
 "JOB"         => $JOB
};

my $TOP = 
{"DATABASE"    => "$name",
 "USER"        => "$name",
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

    if (ref($ydoc) ne "HASH") 
    {
		$self->_error("Badly formed document\n");
    } 
    elsif (!defined($ydoc->{"VERSION"})) 
    {
		$self->_error("required VERSION field is missing\n");
    } 
    else 
    {
		my $version = $ydoc->{"VERSION"};
		if ($version ne "1.0.0.1") 
		{
			$self->_error("Unknown VERSION: $version\n");
		}
		else
		{
			$self->_do_parse($DOCUMENT, "TOP", $ydoc);
			$self->_add_to_document("USER", $ydoc->{"USER"});
			$self->_add_to_document("DATABASE", $ydoc->{"DATABASE"});
		}
    }
	
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
		else 
		{
			# c) input contained some non-array,non-hash type
			return $self->_error("parsetree contains odd datatype " .
								 "$key->(" . ref($value) . ")");
		}
    }
    else
    {
		#   3. we have a mismatch of scalar / pointer
		#      Error: The input file was malformed.
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
    elsif ($key eq "INPUT") 
    {
		$doc->AddInput(Name      => $value->{NAME},
					   Source    => $value->{SOURCE},
					   Columns   => $value->{COLUMNS},
					   Format    => $value->{FORMAT},
					   Delimiter => $value->{DELIMITER});
    } 
    elsif ($key eq "MAP") 
    {
	    $doc->AddMapper(Name => $value->{NAME},
						Language => $value->{LANGUAGE},
						Body => $value->{BODY},
						Optimize => $value->{OPTIMIZE},
						Mode => $value->{MODE});
    }
    elsif ($key eq "TRANSITION") 
    {
		$doc->AddTransition(Name => $value->{NAME},
							Language => $value->{LANGUAGE},
							Body => $value->{BODY},
							Optimize => $value->{OPTIMIZE});
    }
    elsif ($key eq "COMBINER") 
    {
		$doc->AddCombiner(Name => $value->{NAME},
						  Language => $value->{LANGUAGE},
						  Body => $value->{BODY},
						  Optimize => $value->{OPTIMIZE});
    }
    elsif ($key eq "FINALIZER")
    {
		$doc->AddFinalizer(Name => $value->{NAME},
						   Language => $value->{LANGUAGE},
						   Body => $value->{BODY},
						   Optimize => $value->{OPTIMIZE});
    }
    elsif ($key eq "REDUCER") 
    {
		$doc->AddReducer(Name       => $value->{NAME},
                         Initialize => $value->{INITIALIZE},
						 Transition => $value->{TRANSITION},
						 Combiner   => $value->{COMBINE},
						 Finalizer  => $value->{FINALIZE});
    }
    elsif ($key eq "JOB") 
    {
		$err = $doc->add_job($value->{NAME},
							 $value->{SOURCE},
							 $value->{MAPPER},
							 $value->{REDUCER},
							 $value->{GROUPER},
							 $value->{OPTIMIZE});
    }
    elsif ($key eq "RUN") 
    {
		$err = $doc->add_run($value->{SOURCE},
							 $value->{TARGET},
							 $value->{MAPPER},
							 $value->{REDUCER},
							 $value->{MODE});
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


package GPMapReduce;

=head1 NAME

GPMapReduce - The Greenplum Map/Reduce Driver package

=cut

1;
__END__

