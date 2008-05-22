package GPMapReduce::Input;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Input - Map/Reduce Input object

=head1 SYNOPSIS

    use GPMapReduce::Input;
    my $map = new GPMapReduce::Input(Name => 'MyMap');
    $map->SetFunction(
       q[ return {key=> $1, value => $_[1]} if ($_[1] =~ /$_[0]/) ]);
    );

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
    @ISA = qw(Exporter);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT    = qw();
}

################################################################

=head2 @ISA()

See GPMapReduce::Function for full details of inherited methods

=head1 DESCRIPTION

GPMapReduce::Input is the basic INPUT class.  Provides the 
mechanism for loading the data to be processed by later stages,
whether that data comes from a query, view, table, or external
data files.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)
  Creates a new GPMapReduce::Input object, all parameters
  are passed directly into Initialize.

=item Initialize(parameter-list)
  Performs the default initialization of the object.

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    Source   =>    # Where the data comes from
    Columns  =>    # description of columns in an external file
    Format   =>    # format of an external file
    Delimiter =>   # delimiter for an external file

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
    my %param = Params([qw(?Name ?Source ?Format ?Columns ?Delimiter)], @_);
    my $mode = $param{Mode};

    TypeCheck($self, "GPMapReduce::Input", "\$self", 0);
    TypeCheck($param{Name}, "SCALAR", "Name", 0);
    TypeCheck($param{Source}, "SCALAR", "Source", 0);
    TypeCheck($param{Columns}, "SCALAR", "Columns", 1);
    TypeCheck($param{Format}, "SCALAR", "Format", 1);
    TypeCheck($param{Delimiter}, "SCALAR", "Delimiter", 1);

    $self->{Name}      = $param{Name};
    $self->{Source}    = $param{Source};
    $self->{Columns}   = $param{Columns};
    $self->{Format}    = $param{Format};
    $self->{Delimiter} = $param{Delimiter};

    if ($self->{Source} =~ /^(file|gpfdist):\/\/([a-zA-Z:]+)(.*)$/i) {
		$self->{Type}    = uc($1);
		$self->{Host}    = $2;
		$self->{Source}  = $3;
		$self->{Columns} = "value text" if (!defined($self->{Columns}));
		$self->{Format}  = "text" if (!defined($self->{Format}));
    } 
    elsif ($self->{Source} =~ /^exec:(.*)$/i)
    {
		$self->{Type} = "EXEC";
		$self->{Source}  = $1;
		$self->{Columns} = "value text" if (!defined($self->{Columns}));
		$self->{Format}  = "text" if (!defined($self->{Format}));
    }
    elsif ($self->{Source} =~ /^(table|query):(.*)$/i) 
    {
		$self->{Type}    = uc($1);
		$self->{Source}  = $2;
		if (defined($self->{Columns})) {
			Error("$1 does not support support columns");
		}
		if (defined($self->{Format})) {
			Error("$1 does not support support format");
		}
		if (defined($self->{Delimiter})) {
			Error("$1 does not support support delimiter");
		}
    } 
    else
    {
		Error("INPUT <$self->{Name}> has unknown UID $self->{Source}");
    }

    return undef;
}

sub GenerateSQL_Drop
{
    my $self = shift;
    if ($self->{Type} =~ /^(FILE|GPFDIST)/) {
		return "DROP EXTERNAL TABLE $self->{Name} CASCADE;\n";
    } 
    elsif ($self->{Type} == 'EXEC') {
		return "DROP EXTERNAL WEB TABLE $self->{Name} CASCADE;\n";
    }
}

sub GenerateSQL_Create
{
    my $self = shift;
    my $sql;

    if ($self->{Type} =~ /^(FILE|GPFDIST)/) {
		$sql  = "CREATE EXTERNAL TABLE $self->{Name}($self->{Columns})\n";
		$sql .= "  LOCATION('$self->{Type}://$self->{Host}$self->{Source}')\n";
		$sql .= "  FORMAT '$self->{Format}'";
		$sql .= " (DELIMITER $self->{Delimiter})" if (defined($self->{Delimiter}));
		$sql .= ";\n";
    }
    elsif ($self->{Type} == 'EXEC') {
		$sql  = "CREATE EXTERNAL WEB TABLE $self->{Name}($self->{Columns})\n";
		$sql .= "  EXECUTE \$\$ $self->{Source} \$\$\n";
		$sql .= "  FORMAT '$self->{Format}'";
		$sql .= " (DELIMITER $self->{Delimiter})" if (defined($self->{Delimiter}));
		$sql .= ";\n";
    }

    return $sql;
}



################################################################

=head1 SEE ALSO

GPMapReduce::Document

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: cwelton@greenplum.com

=cut

################################################################

1;
__END__


