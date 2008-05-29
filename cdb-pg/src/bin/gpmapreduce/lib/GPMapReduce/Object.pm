package GPMapReduce::Object;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Object - Abstract parent class for all Map/Reduce objects

=head1 SYNOPSIS

	GPMapReduce::Object is an abstract class that should not be 
	directly instantiated.

=head1 REQUIRES

Perl5.5.3, Exporter, Carp, GPMapReduce::Utils

=cut

################################################################

BEGIN
{
    use 5.00503;
	use UNIVERSAL qw(isa);
    use Carp;
	use GPMapReduce::Utils;
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

=head1 DESCRIPTION

GPMapReduce::Object is the abstract class for all Greenplum 
Map/Reduce objects. 

It provides the framework for tracking object dependencies, parameter
checking, and validation.

=head1 METHODS

=head2 Creation

=over 4

=item new(Name => 'name', ...)

  Creates a new GPMapReduce::Object object, the parameters passed to
new will be saved and verified during CheckInput()

=back

=cut

################################################################

sub new {
	my $func = (caller(1))[3];
    my ($class) = shift;
    my $self = bless {}, $class;
	my $inparam;

	# We keep track of our parameter tracking within a hash as follows
	# Both of these hashes will use upcased names to allow easier comparison
	$self->{_Depend} = {};            # Objects we depend on
	$self->{_Params} = {};            # Unparsed values go here
	$self->{_Params}->{Input} = {};   # HASH of input values
	$self->{_Params}->{Check} = {};   # HASH of allowed fields

	# The Check Hash contains fields as follows:
	#   my $p = $self->{_Params}->{Check}->{FIELDNAME};
	#   $p->{Name}     = non-upcased version of fieldname
	#   $p->{Required} = true if it is a required parameter
	#   $p->{Default}  = default value for the field
	#   $p->{Kind}     = typechecking info for the field
	#   $p->{Depend}   = object dependency type

	# We should have recieved our input as a hash, passed either by
	# reference or in a list context.
	if (scalar(@_) == 1 and ref($_[0]) eq 'HASH') {
		$inparam = $_[0];
	} elsif (scalar(@_) % 2 == 0) {
		$inparam = {@_};
	} else {
		confess("Error: Bad Parameter Specification - $func ( @_ )\n");
	}
	
	# upcase all incoming parameters
	for my $key (keys %$inparam) {
		$self->{_Params}->{Input}->{uc($key)} = $inparam->{$key};
	}

	# All objects require a name
	$self->Parameter('Name', Kind => 'name', Required => 1);

    # Bootstrap the name
	if ($self->{_Params}->{Input}->{NAME}) {
		$self->{Name} = $self->{_Params}->{Input}->{NAME};
	} else {
		$self->{Name} = 'Anonymous';
	}

    return $self;
}



################################################################

=head2 Parsing and Validation

=over 4

=item Parameter(self, field, ...)

	Optional parameters
        Required - this field is a required parameter (default = 0)
        Default  - default value for field (default = undef)
        Kind     - datatype that value should be (default based on Default)
        Depend   - type of object value should link to

=cut

################################################################
sub Parameter($$@)
{
	my ($self, $field) = (shift, shift);
	my %params = Params([qw(?Default ?Depend ?Kind ?Required)], @_);
	my $check  = $self->{_Params}->{Check};

	# Validate acceptable parameter names:
	if (not $field =~ /^[a-zA-Z]+$/) {
		confess("Error: Illegal field type: $field");
	}

	# Set defaults
	$params{Required} = 0 unless($params{Required});
	if (not $params{Kind})
	{
		if ($params{Default} and ref($params{Default})) {
			$params{Kind} = ref($params{Default});
		} else {
			$params{Kind} = 'SCALAR';
		}
	}

	# Set the values
	$check->{uc($field)}->{Name}     = $field;
	$check->{uc($field)}->{Required} = $params{Required};
	$check->{uc($field)}->{Kind}     = $params{Kind};
	$check->{uc($field)}->{Depend}   = $params{Depend};
	$check->{uc($field)}->{Default}  = $params{Default};
	
	return undef;
}


################################################################

=item CheckInput(self, %objects)

  1) Checks that all required fields are present
  2) Checks that there are no invalid fields present
  3) Looks up object references and stores the object pointers

=cut

################################################################
sub CheckInput($$)
{
	my ($self, $objects) = (shift, shift);
	my $input = $self->{_Params}->{Input};
	my $check = $self->{_Params}->{Check};
	my ($key, $value);

	# Special case Name so we get better error messages.
	if ($input->{NAME}) {
		$self->{Name} = $input->{NAME};
	} else {
		$self->{Name} = 'Anonymous';
	}

	# Check that no invalid fields are present 
	foreach $key (keys %$input) {
		if (not exists $check->{$key}) {
			confess("Error: $self->{Name} invalid parameter $key");
		}
	}	

	# Check types and assign all fields
	foreach $key (keys %$check) {
		my $name     = $check->{$key}->{Name};
		my $required = $check->{$key}->{Required};
		my $default  = $check->{$key}->{Default};
		my $kind     = $check->{$key}->{Kind};
		my $depend   = $check->{$key}->{Depend};

		# Get the value or set to default, check required
		if (exists $input->{$key}) {
			$value = $input->{$key};
		} elsif (not $required) {
			$value = $check->{$key}->{Default};
		} else {
			confess("Error: $self->{Name} missing parameter $key");
		}
		next if (!defined($value));

		# Type check the value and find dependant objects
		$value = CoerceType($value, $kind);
		if (!$depend) {
			$self->{$name} = $value;
		} elsif ($objects->{uc($value)}) {
			TypeCheck($objects->{uc($value)}, $depend, $key, 0);
			$self->{_Depend}->{uc($value)} = $objects->{uc($value)};
			$self->{$name} = $value;
		} else {
			print "[WARNING] $self->{Name}.$depend $value not listed in YAML\n";
		}
	}
	
	return undef;
}

sub CoerceType($$) {
	my ($value, $type) = @_;

	if (!defined($type)) {
		print "[WARNING] CoerceType($value, undef)\n";
		return $value;
	}

	return undef if (!defined($value));
	return $value if (!ref($value) and $type eq 'SCALAR');
	return $value if (ref($value) eq $type || isa($value, $type));
	return [$value] if (!ref($value) and $type eq 'ARRAY');

	# Handle special values;
	if (!ref($value) and $type eq 'name') {
		return $value if ($value =~ /^[a-zA-Z]\w*$/);
	}
	if (!ref($value) and $type eq 'number') {
		return $value if ($value =~ /^\d+$/);
	}


	confess("Attribute mismatch: $value is not a $type\n");
}


sub GenerateYAML() {
	my $self = shift;
	
	# FIXME - this is scrod, and in no way YAML
	print "{\n";
	foreach my $k (sort keys %$self) {
		if (ref($self->{$k}) eq 'HASH') {
			my $subhash = $self->{$k};
			print "  $k => {";
			foreach my $k2 (keys %$subhash) {
				print "$k2 => $subhash->{$k2} ";
			}
			print "}\n";
		} else {
			print "  $k => $self->{$k}\n";
		}
	}
	print "}\n";
}

# old form, just call the new form, migrate away from this
sub GenerateSQL_Drop { 
	my $self = shift;
	$self->SQLDrop(@_); 
}
sub GenerateSQL_Create { 
	my $self = shift;
	$self->SQLCreate(@_); 
}

# new form
sub SQLDrop { }
sub SQLCreate { }


################################################################

=back

=head1 AUTHORS

Caleb Welton

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__


