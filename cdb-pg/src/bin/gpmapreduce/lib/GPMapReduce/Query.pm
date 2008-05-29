package GPMapReduce::Query;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Query - Map/Reduce Query object

=head1 SYNOPSIS

    use GPMapReduce::Query

=head1 REQUIRES

Perl5.5.3, Exporter, Carp

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA( )

=head1 DESCRIPTION

GPMapReduce::Query - handles collection of the query pipeline

=head1 METHODS

=head2 Creation

=over 4

=item new('myTable', ['key', 'value'])

  Constructs the object.

  Parameters:
    Source sql
    Columns => array of columns returned by sql

=back

=cut

################################################################

sub new($$$) {
    my ($class, $sql, $columns) = @_;
	my $self    = bless {}, $class;

	$self->{Query}   = $sql;
	$self->{Columns} = $columns;
	$self->{Level}   = 1;         

    return $self;
}

sub ExtendQuery($$$$) {
	my ($self, $sql, $columns) = @_;
	my $i;

	if ($self->{Level} > 1) {
		$sql =~ s/<SOURCE>/\($self->{Query}\) q$self->{Level}/;
		$self->{Level}++;
	} else {
		$sql =~ s/<SOURCE>/$self->{Query}/;
	}
	$sql =~ s/<LEVEL>/q$self->{Level}/;
    $self->{Level}++;
	$self->{Query} = $sql;
	$self->{Columns} = $columns;
}

sub FinalizeQuery($) {
	my $self = shift;
	my $sql = "";

	if ($self->{Level} == 1) {
		$sql .= "SELECT " . join(', ', @{$self->{Columns}}) . "\n";
		$sql .= "FROM $self->{Query}";
	} else {
		$sql .= $self->{Query};
	}
	$sql .= "\nORDER BY " . join(', ', @{$self->{Columns}}) . ";\n";
	return $sql;
}

################################################################

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


