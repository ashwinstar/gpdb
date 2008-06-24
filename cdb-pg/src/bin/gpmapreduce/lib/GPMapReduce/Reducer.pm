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

Perl5.5.3, Exporter, GPMapReduce::Object, GPMapReduce::Utils

=cut

################################################################

BEGIN
{
    use 5.00503;
    use GPMapReduce::Object;
    use GPMapReduce::Utils;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Object);
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
  Creates a new GPMapReduce::Reducer object

  Parameters:
     Name        =>    # The name of the object, REQUIRED
     Transition  =>    # Transition function to use, REQUIRED
     Combiner    =>    # Combine function
     Finalizer   =>    # Finalize function
     Initialize  =>    # The initial value for the state

  The datatype and prototype can usually be derived from the transition 
  function if an object is passed, otherwise they will default to
  'text' and 'text, text' respectively.

=back

=cut

################################################################

sub new {
    my ($class) = shift;
    my $self  = new GPMapReduce::Object(@_);

    $self->Parameter('Transition', Required => 1, 
					 Depend => 'GPMapReduce::Function');
    $self->Parameter('Combiner',
					 Depend => 'GPMapReduce::Function');
    $self->Parameter('Finalizer',
					 Depend => 'GPMapReduce::Function');
    $self->Parameter('Initialize'); # Default depends on transition state

    return bless $self, $class;
    return $self;
}


################################################################

=head2 Generation

=over 4

=item SQLCreate
  Produces the SQL output to create the Reducer

=item SQLDrop
  Produces the SQL output to delete the Reducer

=item GenerateYAML
  Unparses the document back into YAML.

=back

=cut

################################################################

sub SQLCreate
{
    my $self = shift;
	my $transition = $self->{_Depend}->{uc($self->{Transition})};
	my @in = @{$transition->{Input_type}};
	my $state = shift @in;
    my $input = join(', ', @in);

    my $sql;
    $sql  = "CREATE AGGREGATE $self->{Name}($input) (\n";
    $sql .= "  stype = $state,\n";
    $sql .= "  initcond = '$self->{Initialize}',\n" if (defined($self->{Initialize}));
    $sql .= "  sfunc = $self->{Transition}";
    $sql .= ",\n  prefunc = $self->{Combiner}" if (defined($self->{Combiner}));
    $sql .= "\n);\n";
    return $sql
}

sub SQLDrop
{
    my $self = shift;
	my $transition = $self->{_Depend}->{uc($self->{Transition})};
	my @in = @{$transition->{Input_type}};
	my $state = shift @in;
    my $input = join(', ', @in);

    return "DROP AGGREGATE IF EXISTS $self->{Name}($input) CASCADE;\n";
}

sub GenerateQuery
{
	my ($self, $input, $params) = @_;
	my ($sql, $i, $j, @out, @in, @columns, @reduceover);
	my $transition = $self->{_Depend}->{uc($self->{Transition})};

	@columns = @{$input->{Columns}};
	@in = @{$transition->{Input_name}};
	@out = ('value');    # FIXME - adjust output parameters

	# Note that the FIRST parameter in the transition input list is the
	# 'state' and isn't passed as a parameter to the reducer.
	shift @in;


	# Unless otherwise specified we group over all columns we didn't
    # take as input. 
	@reduceover = ();
	for ($i = 0; $i <= $#columns; $i++) {
		for ($j = 0; $j <= $#in; $j++) {
			last if (uc($columns[$i]) eq uc($in[$j]));
		}
		push @reduceover, $columns[$i] if ($j > $#in);
	}

	# FIXME: validate that there is at least one field in reduceover

	$sql  = "SELECT " . join(', ', @reduceover) . ", $self->{Name}(";
	$sql .= join(', ', @in) . ") as value\n";
	$sql .= "FROM <SOURCE>\n";
	$sql .= "GROUP BY " . join(', ', @reduceover);
	$input->ExtendQuery($sql, [@reduceover, @out]);

	# FIXME: handle returning multiple columns

	return undef;
}

sub GenerateYAML
{
    my $self = shift;
    my $yml;

    $yml  = "REDUCER: {";
    $yml .= "TRANSITION: $self->{Transition}";
    $yml .= ", COMBINER: $self->{Combiner}" if $self->{Combiner};
    $yml .= ", FINALIZER: $self->{Finalizer}" if $self->{Finalizer};
    $yml .= ", INITIALIZE: $self->{Initialize}" if $self->{Initialize};
    $yml .= "}\n";
    
    return $yml;
}

package GPMapReduce::SUM;

use vars qw(@ISA);
@ISA = qw(GPMapReduce::Reducer);

sub new {
    my ($class) = shift;
    my $self  = new GPMapReduce::Object(Name => 'sum');
    return bless $self, $class;
}

sub SQLCreate()    {}
sub SQLDrop()      {}
sub GenerateYAML() {}


#
# This is exactly the same as for Reducer, except we the input is not
# based on the transition function - since we don't have one declared
#
sub GenerateQuery
{
	my ($self, $input, $params) = @_;
	my ($sql, $i, $j, @out, @in, @columns, @reduceover);
	my $transition = $self->{_Depend}->{uc($self->{Transition})};

	@columns = @{$input->{Columns}};
	@in = ('value');
	@out = ('value');    # FIXME - adjust output parameters

	# Unless otherwise specified we group over all columns we didn't
    # take as input. 
	@reduceover = ();
	for ($i = 0; $i <= $#columns; $i++) {
		for ($j = 0; $j <= $#in; $j++) {
			last if (uc($columns[$i]) eq uc($in[$j]));
		}
		push @reduceover, $columns[$i] if ($j > $#in);
	}

	# FIXME: validate that there is at least one field in reduceover

	$sql  = "SELECT " . join(', ', @reduceover) . ", $self->{Name}(";
	$sql .= join(', ', @in) . ") as value\n";
	$sql .= "FROM <SOURCE>\n";
	$sql .= "GROUP BY " . join(', ', @reduceover);
	$input->ExtendQuery($sql, [@reduceover, @out]);

	# FIXME: handle returning multiple columns

	return undef;
}

################################################################

=head1 SEE ALSO

GPMapReduce::Document, GPMapReduce::Transition, GPMapReduce::Combiner,
GPMapReduce::Finalizer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


