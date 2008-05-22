package GPMapReduce::Finalizer;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Finalizer - Map/Reduce Finalizer object

=head1 SYNOPSIS

    use GPMapReduce::Finalizer;
    my $map = new GPMapReduce::Finalizer(Name => 'MyName');
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

GPMapReduce::Finalizer is the main work method for a
GPMapReduce::Reducer.  

The function for a finalizer should take two (or more) 
parameters where the first parameter is the finalizer state
and the rest of the parameters are the input for the current
tuple.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Finalizer object, all parameters
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

  Input defaults to 'text'
  Output defaluts to 'state text'
  Language defaults to 'perl'

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
    my %param = Params([qw(?Name  ?Language  ?Optimize  ?Body  
			               ?Input ?Output    ?Function)], @_);

    TypeCheck($self, "GPMapReduce::Finalizer", "\$self", 0);

    # Set defaults for Finalizer functions
    $param{Input} = "text" unless (defined $param{Input});
    $param{Output} = "state text" unless (defined $param{Output});

    # Currently we only support simple data type results
    unless ($param{Input} =~ /^\w+(\[\d*\])*$/) {
		Error("Bad Output format: $param{Output}");
    }
    unless ($param{Output} =~ /^\w+\s+\w+(\[\d*\])*$/) {
		Error("Bad Output format: $param{Output}\n");
    }

    # call our parent class Initialize
    GPMapReduce::Function::Initialize($self, %param);
    
    return undef;
}


################################################################

=head2 Inherits

=over 4

=item SetFunction()
=item GenerateSQL_Drop()
=item GenerateSQL_Create()
=item GenerateYAML()

=back

=head1 SEE ALSO

GPMapReduce::Function, GPMapReduce::Document

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: cwelton@greenplum.com

=cut

################################################################

1;
__END__


