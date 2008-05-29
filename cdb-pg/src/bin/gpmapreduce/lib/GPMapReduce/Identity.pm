package GPMapReduce::Identity;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Identity - Map/Reduce Identity object

=head1 SYNOPSIS

    use GPMapReduce::Identity;
    my $input = new GPMapReduce::Identity()

=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Mapper, GPMapReduce::Reducer

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
    use GPMapReduce::Mapper;
	use GPMapReduce::Reducer;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Mapper GPMapReduce::Reducer);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA( GPMapReduce::Mapper GPMapReduce::Reducer )

=head1 DESCRIPTION

GPMapReduce::Identity is the 'do nothing' class for mapping or reducing.

=head1 METHODS

=head2 Creation

=over 4

=item new()

  Constructs the object.

=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self    = new GPMapReduce::Object(Name => 'IDENTITY');
    bless $self, $class;   
    return $self;
}


################################################################

=head2 Generation

=over 4

=item CheckInput
=item SQLDrop
=item SQLCreate
=item GenerateQuery

  Overrides super classes so that they do nothing

=back

=cut

################################################################
sub CheckInput {
	my $self = shift;
	GPMapReduce::Object::CheckInput($self, @_);
}
sub SQLDrop { }
sub SQLCreate { }
sub GenerateQuery($$$)
{
	my ($self, $qry, $params) = @_;
	return $qry;
}


################################################################

=head1 SEE ALSO

GPMapReduce::Document, GPMapReduce::Mapper, GPMapReduce::Reducer

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


