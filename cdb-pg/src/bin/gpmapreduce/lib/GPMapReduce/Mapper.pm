package GPMapReduce::Mapper;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Mapper - Map/Reduce Mapper object

=head1 SYNOPSIS

    use GPMapReduce::Mapper;
    my $map = new GPMapReduce::Mapper(Name => 'MyMap');
    $map->SetFunction(
       q[ return {key=> $1, value => $_[1]} if ($_[1] =~ /$_[0]/) ]);
    );

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

GPMapReduce::Mapper is the basic MAP function class.  It 
describes the map function that will be called within a 
GPMapReduce::Job.

It can be used in one of three ways.

=over 4

=item # Describing the function with a string

You can pass the full body of the function you wish to call
as a string, either during creation or later via the 
SetFunction method.

This method has the advantage that you can describe the
function as being implemented by any supported language.

=item # Describing the function with a function pointer.

Instead of passing a string, you can pass a pointer to
a subfunction.  Obviously this only works for 'perl'
language Mappers.

=item # Creating a subclass of GPMapReduce::Mapper

If a function has not been manually set the Mapper will
inspect itself to see if it has a method name MAP.  If
so it will automatically assign that function as the
map function.  

=back

See GPMapReduce::Function for the list of supported
languages.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Mapper object, all parameters
  are passed directly into Initialize.

=item Initialize(parameter-list)
  Performs the default initialization of the object.

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Language =>    # The implementation language 
    Body     =>    # Text describing the body of the function
    Optimize =>    # Optimization flags for the function
    Mode     =>    #   separate from optimize why?
    Input    =>    # Describes input parameters
    Output   =>    # Describes input parameters
    Function =>    # Function pointer for the function

  Input defaults to 'text, text'
  Output defaluts to 'key text, value text'
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
    my %param = Params([qw(?Name  ?Language ?Optimize  ?Body ?Mode 
                           ?Input ?Output   ?Function)], @_);
    my $mode = $param{Mode};

    TypeCheck($self, "GPMapReduce::Mapper", "\$self", 0);
    TypeCheck($mode, "SCALAR", "Mode", 1);

    # FIXME: switch to default MULTI, move SINGLE to optimize
    $mode = "SINGLE" unless(defined $mode);
    $self->SetMode('Mode' => $mode);
    delete $param{Mode};

    # Set defaults for Mapper functions
    $param{Input} = "text, text" unless (defined $param{Input});
    $param{Output} = "key text, value text" unless (defined $param{Output});

    # call our parent class Initialize
    GPMapReduce::Function::Initialize($self, %param);
    
    return undef;
}


# Mode is just a big mess and should be abolished
sub SetMode()
{
    my $self = shift;
    my %param = Params([qw(Mode)], @_);
    my $mode = $param{Mode};

# FIXME: Remove legacy DOCMAP support
    if ($mode eq 'DOCMAP') {
		$self->{DOCMAP} = 1;
		$mode = 'SINGLE';
    }

    unless ($mode =~ /^(single|multi)$/i) {
		Error("(Mode => $mode) not allowed");
    }

    $self->{Mode} = uc($mode);
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


