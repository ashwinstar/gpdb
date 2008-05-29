package GPMapReduce::Output;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Output - Map/Reduce Output object

=head1 SYNOPSIS

    use GPMapReduce::Output
    my $input = new GPMapReduce::Output(Name => 'myOut')

=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Object

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
    use GPMapReduce::Object;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Object);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA( GPMapReduce::Object )

=head1 DESCRIPTION

GPMapReduce::Output - handles the end of a query pipeline

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)

  Constructs the object.

  Parameters:
    Name   =>   # The Name of the object, REQUIRED
    File   =>   # File to output to
	Table  =>   # Table to output to
    Mode   =>   # REPLACE | APPEND

	File and Table are mutually exclusive parameters, one of which
	must be specified.


=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self    = new GPMapReduce::Object(@_);
    bless $self, $class;

	$self->Parameter('File');
	$self->Parameter('Table');
	$self->Parameter('Mode');

    return $self;
}

sub CheckInput
{
	my ($self, $objects) = (shift, shift);

	# Call superclass
	GPMapReduce::Object::CheckInput($self, $objects);

	if ($self->{File} and $self->{Table}) {
		confess("Error: Output <$self->{Name}> specifies both TABLE and FILE");
	}
	if (!$self->{File} and !$self->{Table}) {
		confess("Error: Output <$self->{Name}> requires TABLE or FILE");
	}
	if (defined($self->{Mode}) and
		not ($self->{Mode} =~ /^(REPLACE|APPEND)$/i)) {
		confess("Error: Output <$self->{Name}> has bad MODE => $self->{Mode}");
	}

	return undef;
}


################################################################

=head2 Generation

=over 4

=item GenerateQuery

  Handles directing output to the correct location

=back

=cut

################################################################
sub GenerateQuery($$$)
{
	my ($self, $input, $params) = @_;
	my ($sql);

	if ($self->{Table}) {
		$input->ExtendQuery("CREATE TABLE $self->{Table} AS <SOURCE>",
							['key', 'value']);
	} else {
		# FIXME: If we aren't using COPY then we need something else
	}
	return undef;
}


################################################################

=head1 SEE ALSO

GPMapReduce::Document

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;

package GPMapReduce::STDOUT;
use strict;
use warnings;
BEGIN
{
    use 5.00503;
    use Carp;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Output);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

sub new {
    my ($class) = shift;
	my $self    = new GPMapReduce::Object(Name => 'STDOUT');
    bless $self, $class;
    return $self;
}

# Use the default Object::CheckInput, not the Output::CheckInput
sub CheckInput
{
	my ($self, $objects) = @_;
	GPMapReduce::Object::CheckInput($self, $objects);
}

__END__


