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

  Creates a new GPMapReduce::Mapper object

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Language =>    # The implementation language
    Body     =>    # Text describing the body of the function
    Function =>    # Function pointer for the function 
    Optimize =>    # Optimization flags for the function
    Input    =>    # Describes input parameters
    Output   =>    # Describes input parameters

  Input defaults to 'text, text'
  Output defaluts to 'key text, value text'
  Language defaults to 'perl'
  Body and Function are mutually exclusive

=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self = new GPMapReduce::Function(@_);
	bless $self, $class;   

	# Override Function Input/Output with Mapper defaults
	$self->Parameter('Input', Default => ['key text', 'value text']);
	$self->Parameter('Output', Default => ['key text', 'value text']);
	
    return $self;
}



################################################################

=head2 Query Generation

=over 4

=item SQLDrop()
=item SQLCreate()
=item GenerateQuery()

=back

=cut

################################################################

sub GenerateQuery($$$)
{
	my ($self, $input, $params) = @_;
	my ($key, $sql, $i, $j, @in, @out, @intype, @columns);

	if ($#{$params} >= 0 and defined(${$params}[0])) {
		$key = "'${$params}[0]'";
	} else {
		$key = undef;
	}

   	# FIXME - adjust input/output parameters
	@columns = @{$input->{Columns}};
	@in = @{$self->{Input_name}};
	@intype = @{$self->{Input_type}};
	@out = @{$self->{Output_name}};

	# Special case input column named 'key':
	#   1) If there is a column in the source called key, use it
	#   2) Otherwise use a parameter from the command line
	#   3) If no parameters use NULL
	for ($i = 0; $i <= $#in; $i++) 
	{
		if (uc($in[$i]) eq 'KEY') 
		{
			for ($j = 0; $j <= $#columns; $j++) 
			{
				last if (uc($columns[$j]) eq 'KEY');
			}
			if ($j > $#columns) 
			{
				if (defined($key)) { 
					$in[$i] = "$key\::$intype[$i]";
				} else { 
					$in[$i] .= "NULL::$intype[$i]"; 
				}
			}
		}
	}

	# Generate the query 
	if ($#out == 0) {
		$sql = "SELECT $self->{Name}(";
		$sql .= join(', ', @in);		
		$sql .= ") as $out[0] FROM <SOURCE>\n";
	} else {
		$sql = "SELECT ";
		$sql .= join('(m), ', @out);
		$sql .= "(m) FROM (SELECT $self->{Name}(";
		$sql .= join(', ', @in);
		$sql .= ") as m FROM <SOURCE>) <LEVEL>\n";
		$sql .= "WHERE m is not null"
	}

	$input->ExtendQuery($sql, [@out]);

	return $sql;
}


################################################################

=head2 Inherits

=over 4

=item CheckInput()
=item SetFunction()
=item GenerateYAML()

=back

=head1 SEE ALSO

GPMapReduce::Object, GPMapReduce::Function, GPMapReduce::Document

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


