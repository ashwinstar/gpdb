package GPMapReduce::Reducer;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Reducer - Map/Reduce Reducer object

=head1 SYNOPSIS

    use GPMapReduce::Document;
    use GPMapReduce::Reducer;
    my $doc = new GPMapReduceDocument();
    my $reducer = new
       GPMapReduce::Reducer(Name => 'MyName',
			    Transition => 'myTransition',
			    Combiner => 'myCombiner',
			    Finalizer => 'myFinalizer');
    $doc->AddReducer($reducer);

=head1 REQUIRES

Perl5.5.3, Exporter, GPMapReduce::Utils

=cut

################################################################

BEGIN
{
    use 5.00503;
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

=head1 DESCRIPTION

GPMapReduce::Reducer describes a Reduce operation.  The Reduce
operator for Greenplum uses a state/transition function model.
For optimal parallism the user should provide a combine function
as well when possible.

A Reducer is described by:

=over 4

=item A State
=item The initial value for the state
=item A Transition function
=item A Combine function (optional)
=item A Finalize function (optional)

=back

Currently the State can only be a simple type (not a tuple).

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Reducer object, all parameters
  are passed directly into Initialize.

=item Initialize(parameter-list)
  Performs the default initialization of the object.

  Parameters:
     Name        =>    # The name of the object, REQUIRED
     Transition  =>    # Transition function to use, REQUIRED
     Init        =>    # The initial value for the state
     Combiner    =>    # Combine function
     Datatype    =>    # The datatype of the state
     Prototype   =>    # Input parameters

  The datatype and prototype can usually be derived from the transition 
  function if an object is passed, otherwise they will default to
  'text' and 'text, text' respectively.

=back

=cut

################################################################

sub new {
    my ($class) = shift;
    my $self = bless {}, $class;   
    $self->Initialize(@_);
    return $self;
}

sub Initialize {
    my $self  = shift;
    my %param = Params([qw(?Name ?Transition ?Initialize ?Combiner 
                           ?Finalizer ?Datatype ?Prototype)], @_);
    my $name      = $param{Name};
    my $trans     = $param{Transition};
    my $init      = $param{Initialize};
    my $combine   = $param{Combiner};
    my $final     = $param{Finalizer};

    # FIXME: validate reasonable datatype/prototype
    my $datatype  = lc($param{Datatype});
    my $prototype = lc($param{Prototype});

    TypeCheck($self, "GPMapReduce::Reducer", "\$self", 0);
    TypeCheck($init, "SCALAR", "Initialize", 1);
    TypeCheck($datatype, "SCALAR", "Datatype", 1);
    TypeCheck($prototype, "SCALAR", "Prototype", 1);

    # Reducers, Combiners and Finalizers can be given by name or by object.
    if (ref($trans)) {
	TypeCheck($trans, "GPMapReduce::Transition", "Transition", 0);
	
	# check datatype/transition return
	if (!defined($datatype)) {
	    $datatype = lc($trans->{Output});
	} elsif ($datatype ne lc($trans->{Output})) {
		Error("(Datatype => $datatype)\n" .
			  "  Supplied datatype for $name does not match the return\n" .
			  "  value of the supplied transition function $trans->{Name}\n");
	}

	# check prototype vs datatype and transition function
	if (!defined($prototype)) {
	    $prototype = lc("$datatype, $trans->{Input}");
	} elsif ($prototype ne lc("$datatype, $trans->{Input}")) {
		Error("(Prototype => $prototype)\n" .
			  "  Supplied prototype for $name does not match the input\n" .
			  "  types of the supplied transition function $trans->{Name}\n");
	}

	# Save the transition object and change $trans to just the name.
	$self->{TransObj} = $trans;
	$trans = $trans->{Name};
    } else {
		TypeCheck($trans, "SCALAR", "Transition", 0);
    }

    if (ref($combine)) {
		TypeCheck($combine, "GPMapReduce::Combiner", "Combiner", 1);
		$self->{CombineObj} = $combine;
		$combine = $combine->{Name};
    } else {
		TypeCheck($combine, "SCALAR", "Combiner", 1);
    }

    if (ref($final)) {
		TypeCheck($final, "GPMapReduce::Finalizer", "Finalizer", 0);
	$self->{FinalObj} = $final;
	$final = $final->{Name};
    } else {
		TypeCheck($final, "SCALAR", "Finalizer", 1);
    }
    
    $self->{Name}       = $name;
    $self->{Transition} = $trans;
    $self->{Initial}    = $init;
    $self->{Combiner}   = $combine;
    $self->{Finalizer}  = $final;
    $self->{Datatype}   = $datatype;
    $self->{Prototype}  = $prototype;

    return $self;
}


################################################################

=head2 Generation

=over 4

=item GenerateSQL_Create
  Produces the SQL output to create the Reducer

=item GenerateSQL_Drop
  Produces the SQL output to delete the Reducer

=item GenerateYAML
  Unparses the document back into YAML.

=back

=cut

################################################################

sub GenerateSQL_Create
{
    my $self = shift;
    my $input = 'text';   # FIXME
    my $state = 'text';   # FIXME

    my $sql;
    $sql  = "CREATE AGGREGATE $self->{Name}($input) (\n";
    $sql .= "  stype = $state,\n";
    $sql .= "  initcond = '$self->{Initial}',\n";
    $sql .= "  sfunc = $self->{Transition}";
    $sql .= ",\n  prefunc = $self->{Combiner}" if (defined($self->{Combiner}));
    $sql .= "\n);\n";
    return $sql
}

sub GenerateSQL_Drop
{
    my $self = shift;
    my $input = 'text';   # FIXME

    return "DROP AGGREGATE $self->{Name}($input) CASCADE;\n";
}

sub GenerateYAML
{
	Error("NOT IMPLEMENTED");
}


################################################################

=head1 SEE ALSO

GPMapReduce::Document, GPMapReduce::Transition, GPMapReduce::Combiner,
GPMapReduce::Finalizer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: cwelton@greenplum.com

=cut

################################################################

1;
__END__


