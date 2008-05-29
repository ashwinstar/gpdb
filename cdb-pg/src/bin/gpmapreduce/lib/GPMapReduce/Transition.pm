package GPMapReduce::Transition;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Transition - Map/Reduce Transition object

=head1 SYNOPSIS

    use GPMapReduce::Transition;
    my $map = new GPMapReduce::Transition(Name => 'MyName');
    $map->SetFunction(q[ return {state=> $_[0]+$_[1]}; ]);

=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Utils,
GPMapReduce::Function

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
    use GPMapReduce::Utils;
    use GPMapReduce::Function;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Function);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA(GPMapReduce::Function)

See GPMapReduce::Function for full details of inherited methods

=head1 DESCRIPTION

GPMapReduce::Transition is the main work method for a
GPMapReduce::Reducer.  

The function for a transition should take two (or more) 
parameters where the first parameter is the transition state
and the rest of the parameters are the input for the current
tuple.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Transition object, all parameters
  are passed directly into Initialize.

=item Initialize(parameter-list)
  Performs the default initialization of the object.

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Language =>    # The implementation language 
    Body     =>    # Text describing the body of the function
    Optimize =>    # Optimization flags for the function
    Input    =>    # Describes input parameters
    Output   =>    # Describes output parameters
    Function =>    # Function pointer for the function

  Input defaults to 'text, text'
  Output defaluts to 'state text'
  Language defaults to 'perl'

=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self = new GPMapReduce::Function(@_);
	bless $self, $class;   

	# Override Function Input/Output with Transition defaults
	$self->Parameter('Input', Default => ['state text', 'value text']);
	$self->Parameter('Output', Default => ['state text']);

    return $self;
}


################################################################

=head2 Inherits

=over 4

=item CheckInput()
=item SetFunction()
=item SQLDrop()
=item SQLCreate()
=item GenerateYAML()

=back

=head1 SEE ALSO

GPMapReduce::Function, GPMapReduce::Document

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


