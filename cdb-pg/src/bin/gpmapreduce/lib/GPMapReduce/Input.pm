package GPMapReduce::Input;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Input - Map/Reduce Input object

=head1 SYNOPSIS

    use GPMapReduce::Input;
    my $input = new GPMapReduce::Input(
	     Name => 'MyInput',
         File => ['localhost:/path/to/file'],
    );
    ...
	$input->CheckInput($objecthash);


=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Object, GPMapReduce::Utils
GPMapReduce::Query

=cut

################################################################

BEGIN
{
    use 5.00503;
    use Carp;
    use GPMapReduce::Object;
	use GPMapReduce::Query;
    use GPMapReduce::Utils;
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

GPMapReduce::Input is the basic INPUT class.  Provides the 
mechanism for loading the data to be processed by later stages,
whether that data comes from a query, view, table, or external
data files.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)

  Parameters:
    Name     =>    # The name of the object, REQUIRED
    File     =>    # Where the data comes from (exlusive with table/query)
    Table    =>    # Where the data comes from (exlusive with file/query)
    Query    =>    # Where the data comes from (exlusive with file/table)
    Columns  =>    # description of columns in an external file
    Format   =>    # format of an external file
    Delimiter =>   # delimiter for an external file

=back

=cut

################################################################

sub new {
    my ($class) = shift;
	my $self    = new GPMapReduce::Object(@_);
    bless $self, $class;   

	$self->Parameter('File', Kind => 'ARRAY');
	$self->Parameter('Table', Kind => 'SCALAR');
	$self->Parameter('Query', Kind => 'SCALAR');
	$self->Parameter('Columns', Default => ['value text']);
	$self->Parameter('Format', Default => 'text');
	$self->Parameter('Delimiter', Kind => 'SCALAR');

    return $self;
}

sub CheckInput($$) {
	my ($self, $objects) = (shift, shift);

	# Call superclass
	GPMapReduce::Object::CheckInput($self, $objects);

	# Perform GPMapReduce::Input specific verification
	if (($self->{File} and ($self->{Table} or $self->{Query})) or
		($self->{Table} and $self->{Query})) {
		Error("Input can only specify one of: FILE/TABLE/QUERY");
	}

	if ($self->{File}) {
		$self->{Type} = "FILE";
		$self->{Source} = '';
		foreach my $file (@{$self->{File}}) {
			if ($file =~ /^([a-zA-Z]+):(.*)$/) {
				my ($host, $path) = ($1, $2);
				if (not $self->{Source} eq '') {
					$self->{Source} .= ",\n           ";
				}
				$self->{Source} .= "'file://$host$path'";
			} else {
				Error("Invalid FILE specification: '$file'");
			}
		}
	} elsif ($self->{Table}) {
		if ($self->{Table} =~ /^[a-zA-Z]\w+$/) {
			$self->{Type} = "TABLE";
			$self->{Source} = $self->{Table};
		} else {
			Error("Invalid TABLE specification: $self->{Table}");			
		}
		if (defined($self->{Columns})) {
			Error("INPUT.TABLE does not support support columns");
		}
		if (defined($self->{Format})) {
			Error("INPUT.TABLE does not support support format");
		}
		if (defined($self->{Delimiter})) {
			Error("INPUT.TABLE does not support support delimiter");
		}
	} elsif ($self->{Query}) {
		$self->{Type} = "QUERY";
		$self->{Source} = $self->{Query};
		if (defined($self->{Columns})) {
			Error("INPUT.QUERY does not support support columns");
		}
		if (defined($self->{Format})) {
			Error("INPUT.QUERY does not support support format");
		}
		if (defined($self->{Delimiter})) {
			Error("INPUT.QUERY does not support support delimiter");
		}
	} else {
		Error("INPUT <$self->{Name}> requires FILE, QUERY or TABLE");
	}
    return undef;
}


sub SQLDrop
{
    my $self = shift;
    if ($self->{Type} =~ /^(FILE|GPFDIST)/) {
		return "DROP EXTERNAL TABLE IF EXISTS $self->{Name} CASCADE;\n";
    } 
    elsif ($self->{Type} eq 'EXEC') {
		return "DROP EXTERNAL WEB TABLE IF EXISTS $self->{Name} CASCADE;\n";
    }
}

sub SQLCreate
{
    my $self = shift;
    my $sql;

    if ($self->{Type} =~ /^(FILE|GPFDIST)/) {
		$sql  = "CREATE EXTERNAL TABLE $self->{Name}(";
		$sql .= join(', ', @{$self->{Columns}});
		$sql .= ")\n";
		$sql .= "  LOCATION($self->{Source})\n";
		$sql .= "  FORMAT '$self->{Format}'";
		$sql .= " (DELIMITER '$self->{Delimiter}')" if (defined($self->{Delimiter}));
		$sql .= ";\n";
    }
    elsif ($self->{Type} eq 'EXEC') {
		$sql  = "CREATE EXTERNAL WEB TABLE $self->{Name}($self->{Columns})\n";
		$sql .= "  EXECUTE \$\$ $self->{Source} \$\$\n";
		$sql .= "  FORMAT '$self->{Format}'";
		$sql .= " (DELIMITER $self->{Delimiter})" if (defined($self->{Delimiter}));
		$sql .= ";\n";
    }

    return $sql;
}

sub GenerateQuery($$)
{
	my ($self, $params) = @_;
	my $columns = [];

	# our stored columns are in <name type> pairs, extract just the names
	foreach my $name (@{$self->{Columns}}) {
		my @pair = split(' ', $name);
		push @$columns, $pair[0];
	}

	# Produce and return the initial query.
	if ($self->{Type} =~ /^(FILE|GPFDIST)/) {
		return new GPMapReduce::Query($self->{Name}, $columns);
	} else {
		return new GPMapReduce::Query($self->{Source}, $columns);
	}
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
__END__


