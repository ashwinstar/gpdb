package GPMapReduce::Task;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Task - Map/Reduce Task object

=head1 SYNOPSIS

    use GPMapReduce::Task;
    my $task = new GPMapReduce::Task(Name => 'MyTask', 
                                     Source => 'myInput');

=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Object, GPMapReduce::Utils
GPMapReduceInput

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
	use GPMapReduce::Input;
    use GPMapReduce::Utils;
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION   = '1.00';
    @ISA = qw(Exporter GPMapReduce::Input);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA(GPMapReduce::Input)

=head1 DESCRIPTION

GPMapReduce::Task is the basic TASK class.  

It provides the glue connecting a given Input, Map, Reduce, and Output.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Source   =>    # Where the data comes from, REQUIRED
    Mapper   =>    # How the input should be mapped
    Reducer  =>    # How the data should be reduced
    Output   =>    # Where the data is going

  Creates a new GPMapReduce::Task object

  Parameters are not validated until CheckInput is called

=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self = new GPMapReduce::Object(@_);
    bless $self, $class;

	$self->Parameter('Source', Depend => 'GPMapReduce::Input', Required => 1);
	$self->Parameter('Mapper', Depend => 'GPMapReduce::Mapper',
					 Default => 'IDENTITY');
	$self->Parameter('Reducer', Depend => 'GPMapReduce::Reducer',
					 Default => 'IDENTITY');
	$self->Parameter('Output', Depend => 'GPMapReduce::Output',
					 Default => 'STDOUT');
    return $self;
}

sub CheckInput($$) {
	my ($self, $objects) = (shift, shift);

	# Call Object::CheckInput instead of Input::CheckInput
	GPMapReduce::Object::CheckInput($self, $objects);
}

sub SQLDrop
{
    my $self = shift;
	return "DROP VIEW IF EXISTS $self->{Name} CASCADE;\n";
}

sub SQLCreate
{
    my ($self, $params) = (shift, shift);
    my $sql;

	$sql  = "CREATE VIEW $self->{Name} AS\n";
	$sql .= $self->BuildQuery($params);   # params??
    return $sql;
}

sub GenerateQuery()
{
	my ($self,$params) = @_;
	my $source  = $self->{_Depend}->{uc($self->{Source})};
	my $mapper  = $self->{_Depend}->{uc($self->{Mapper})};
	my $reducer = $self->{_Depend}->{uc($self->{Reducer})};
	my $output  = $self->{_Depend}->{uc($self->{Output})};
	my $out;

	# Columns are defined by the last stage of the pipeline
	if ($output->{Output_name}) {
		$out = $output->{Output_name};
	} elsif ($reducer->{Output_name}) {
		$out = $reducer->{Output_name};
	} elsif ($mapper->{Output_name}) {
		$out = $mapper->{Output_name};
	} elsif ($source->{Output_name}) {
		$out = $source->{Output_name};
	} else {
		$out = ['value'];  # ???
	}

	return new GPMapReduce::Query("$self->{Name}", $out);
}

sub BuildQuery
{
	my ($self,$params) = @_;
	my $source  = $self->{_Depend}->{uc($self->{Source})};
	my $mapper  = $self->{_Depend}->{uc($self->{Mapper})};
	my $reducer = $self->{_Depend}->{uc($self->{Reducer})};
	my $output  = $self->{_Depend}->{uc($self->{Output})};
	my $query;

	$query = $source->GenerateQuery($params);
	$mapper->GenerateQuery($query, $params);
	$reducer->GenerateQuery($query, $params);
	$output->GenerateQuery($query, $params);

	return $query->FinalizeQuery();
}

################################################################

=head1 SEE ALSO

GPMapReduce::Document
GPMapReduce::Input
GPMapReduce::Mapper
GPMapReduce::Reducer
GPMapReduce::Output

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


