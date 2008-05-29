package GPMapReduce::Combiner;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Combiner - Map/Reduce Combiner object

=head1 SYNOPSIS

    use GPMapReduce::Combiner;
    my $map = new GPMapReduce::Combiner(Name => 'MyName');
    $map->SetFunction(q[ return {state=> $_[0]+$_[1]}; ]);

=head1 REQUIRES

Perl5.5.3, Exporter, GPMapReduce::Utils, GPMapReduce::Function

=cut

################################################################

BEGIN
{
    use 5.00503;
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

GPMapReduce::Combiner is used to accumulate the output from
GPMapReduce::Reducers.  It can be used to allow reducers to
calculate subresults locally and only ship summary data over
the network.  If the operation you want to do allows you 
should try to define a combiner for optimal performance.

The function for a combiner should take exactly two parameters 
of the same datatype and return the same datatype.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Combiner object, all parameters
  are passed directly into Initialize.

=item Initialize(parameter-list)
  Performs the default initialization of the object.

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Language =>    # The implementation language 
    Body     =>    # Text describing the body of the function
    Optimize =>    # Optimization flags for the function
    Input    =>    # Describes input parameters
    Output   =>    # Describes out parameters
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

	# Override Function Input/Output with Mapper defaults
	$self->Parameter('Input', Default => 'text, text');
	$self->Parameter('Output', Default => 'state text');

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

GPMapReduce::Function, GPMapReduce::Document, GPMapReduce::Reducer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


