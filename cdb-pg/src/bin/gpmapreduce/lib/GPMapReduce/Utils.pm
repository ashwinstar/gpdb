package GPMapReduce::Utils;

use 5.00503;
use strict;
use warnings;

require Exporter;
use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
@ISA = qw(Exporter);

# This allows declaration	use GPMapReduce::MapReduce ':all';
%EXPORT_TAGS = ( 'all' => [ qw() ] );
@EXPORT_OK   = ( @{ $EXPORT_TAGS{'all'} } );
@EXPORT      = qw(TypeCheck Params Error);
$VERSION     = '0.01';

use UNIVERSAL qw(isa);
use Carp;

sub Params {
	my $func    = (caller(1))[3];
	my $params  = shift;
	my $result  = {};
	my %actual;

	if (ref($params) ne 'ARRAY') {
		my $r = ref($params);
		confess("Error: Bad Parameter Specification - $func( $r )\n");
	}

	if (scalar(@_) % 2 == 1) {
		confess("Error: Function input not passed by name - $func( @_ )");
	}
	%actual = @_;


	# 1) Check valid parameter specification
	foreach my $p (@$params) {
		if (not ($p =~ /^(\??)(\w+)$/)) {
			confess("Error: Bad Parameter Specification - $func( $p )\n");
		}
		# For required parameters we set the parameter to a value that
		# could not possibly have been passed in - the address of our
		# result hash.  This is just a dummy value, we will verify that
		# it has been overridden before returning.
		if ($1) {
			$result->{$2} = undef;
		} else {
			$result->{$2} = $result;   # required parameter
		}
	}

	# 1) Ensure that all passed in keys are valid
	foreach my $a (keys %actual) {
		my $found = 0;
		foreach my $p (keys %$result) {
			if (lc($a) eq lc($p)) {
				$result->{$p} = $actual{$a};
				$found = 1;
				last;
			}
		}
		if (not $found) {
			confess("Error: Unknown Parameter - $func( $a )\n");
		}
	}

	# 2) Ensure that all required parameters are present
	foreach my $p (keys %$result) {
		if (not defined($result->{$p})) {
			delete $result->{$p};
			next;
		}
		if ($result->{$p} eq $result) {
			confess("Error: Required Parameter missing - $func( $p )\n");
		}
	}

	return %$result;
}

sub TypeCheck {
	my $func     = (caller(1))[3];
    my ($obj, $type, $name, $oknull) = @_;
    my $got = "undef";

    return 1 if (!defined($obj) and $oknull);
    if (defined($obj)) {
	return 1 if (!ref($obj) and $type eq 'SCALAR');  
	return 1 if (ref($obj) eq $type or isa($obj, $type));
	$got = ref($obj) ? ref($obj) : "SCALAR";
    }
    confess("Error: Parameter Type Mismatch\n" .
	    "       $func( $name => $got :: expected $type )\n");
}

sub Error {
	my $func = (caller(1))[3];
	my $e = shift;
	croak("Error: $e\n    $func\n");
}


1;
__END__
