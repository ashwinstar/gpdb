package GPMapReduce::Document;
use strict;
use warnings;

################################################################

=head1 NAME

GPMapReduce::Document - Map/Reduce Document Container Class

=head1 SYNOPSIS

    use GPMapReduce::Document;                   
    my $doc = GPMapReduce::Document->new();       
    $doc->AddInput($input);                      
    $doc->AddMapper($map);                       
    $doc->AddReducer($reduce); 

    $doc->Execute(Explain => $explain);
    print $doc->GenerateSQL();
    print $doc->GenerateYAML();

=head1 REQUIRES

Perl5.5.3, Exporter, GPMapReduce::Utils GPMapReduce::Input, 
GPMapReduce::Mapper, GPMapReduce::Transition GPMapReduce::Combiner, 
GPMapReduce::Finalizer

=cut

################################################################

BEGIN 
{
    use 5.00503;
    use GPMapReduce::Utils;
    use GPMapReduce::Input;
	use GPMapReduce::Output;
    use GPMapReduce::Mapper;
    use GPMapReduce::Transition;
    use GPMapReduce::Combiner;
    use GPMapReduce::Finalizer;
    use GPMapReduce::Reducer;
	use GPMapReduce::Task;
	use GPMapReduce::Identity;
    use UNIVERSAL qw(isa);
    require Exporter;

    use vars qw($VERSION @ISA @EXPORT @EXPORT_OK %EXPORT_TAGS);
    $VERSION     = '1.00';
    @ISA         = qw(Exporter);
    %EXPORT_TAGS = ( 'all' => [ qw() ] );
    @EXPORT_OK   = ( @{ $EXPORT_TAGS{'all'} } );
    @EXPORT      = qw();
}

################################################################

=head1 DESCRIPTION

GPMapReduce::Document is a collector class that holds a set of inputs,
outputs, mappers, and reducers. 

It validates objects, manages the namespace, and generates the code
to execute the document.

=head1 METHODS

=head2 Creation

=over 4

=item new()

  Creates a new GPMapReduce::Document instance.

=back

=cut

################################################################

sub new() 
{ 
    my $class = shift;
    my $self = bless {}, $class;
    $self->initialize(@_);
    return $self;
}

sub initialize() 
{
    my $self = shift;
    my @param = @_;

    $self->{DATABASE} = undef;
    $self->{USER}     = undef;
	$self->{OBJECTS}  = { 
		IDENTITY => new GPMapReduce::Identity(),
		STDOUT   => new GPMapReduce::STDOUT(),
	};
    $self->{INPUTS}   = [];
    $self->{FUNCS}    = [];
    $self->{MAPS}     = [];
    $self->{REDUCES}  = [];
    $self->{TASKS}     = [];
    $self->{RUN}      = [];
    $self->{PARAM}    = [@param];    ## Fixme, this isn't the cleanest

    return undef;
}

################################################################

=head2 Attributes

=over 4

=item SetDatabase(dbname)

  Sets that database to connect to

=item SetUser(uname)

  Sets the user to connect as

=item SetHost(hostname)

  Sets the host to connect to

=item SetPort(uname)

  Sets the port to connect on

=item SetVersion(version)

  Sets the version for YAML generation

=back

=cut

################################################################

sub SetDatabase() {
    my ($self, $value) = @_;
	if ($value) {
		chomp($value);
		$self->{"DATABASE"} = $value;
	} else {
		$self->{"DATABASE"} = undef;
	}
    return undef;
}

sub SetUser() {
    my ($self, $value) = @_;
	if ($value) {
		chomp($value);
		$self->{"USER"} = $value;
	} else {
		$self->{"USER"} = undef;
	}
    return undef;
}

sub SetHost() {
    my ($self, $value) = @_;
	if ($value) {
		chomp($value);
		$self->{"HOST"} = $value;
	} else {
		$self->{"HOST"} = undef;
	}
    return undef;
}

sub SetPort() {
    my ($self, $value) = @_;
	if ($value) {
		chomp($value);
		$self->{"PORT"} = $value;
	} else {
		$self->{"PORT"} = undef;
	}
    return undef;
}

sub SetVersion() {
    my ($self, $value) = @_;
	if ($value) {
		chomp($value);
		$self->{"VERSION"} = $value;
	} else {
		$self->{"VERSION"} = undef;
	}
    return undef;
}


################################################################

=head2 Collection

=over 4

=item AddInput(Input)

  Adds a GPMapReduce::Input to the Document.

=item AddSplitter(Split)

  Adds a GPMapReduce::Splitter to the Document.

=item AddMapper(Mapper)    

  Adds a GPMapReduce::Mapper to the Document.

=item AddReducer(Reducer)    

  Adds a GPMapReduce::Reducer to the Document.

=item AddTransition(Transition)

  Adds a GPMapReduce::Transition to the Document.

=item AddCombiner(Combiner)

  Adds a GPMapReduce::Combiner to the Document.

=item AddFinalizer(Finalizer)

  Adds a GPMapReduce::Finalizer to the Document.

=item AddTask(Finalizer)

  Adds a GPMapReduce::Task to the Document.


=back

=cut

################################################################

sub AddInput
{
    my $self   = shift;
    my %param  = Params([qw(?Input ?Name ?File ?Table ?Query 
                            ?Format ?Columns ?Delimiter)], @_);
    my $input  = $param{"Input"};
    TypeCheck($self, "GPMapReduce::Document", "\$self", 0);
    TypeCheck($input, "GPMapReduce::Input", "Input", 1);

	# Remove parameters with undef values
	delete $param{File}  if (!defined $param{File});
	delete $param{Table} if (!defined $param{Table});
	delete $param{Query} if (!defined $param{Query});

    if (!defined $input) {
		delete $param{"Input"};
		$input = new GPMapReduce::Input(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify an Input and other arguments");
    }

	$self->{OBJECTS}->{uc($input->{Name})} = $input;
    push @{$self->{INPUTS}}, $input;
    return undef;
}

sub AddMapper
{
    my $self   = shift;
    my %param  = Params([qw(?Mapper ?Name   ?Language ?Function  ?Optimize 
                            ?Mode   ?Input  ?Output   ?Body)], @_);
    my $mapper = $param{"Mapper"};
    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($mapper, "GPMapReduce::Mapper", "Mapper", 1);
    
    # We should have either recieved a real mapper and nothing else,
    # OR enough parameters to make one.
    if (!defined $mapper) {
		delete $param{"Mapper"};
		$mapper = new GPMapReduce::Mapper(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Mapper and other arguments");
    }

	$self->{OBJECTS}->{uc($mapper->{Name})} = $mapper;
    push @{$self->{FUNCS}}, $mapper;
    return undef;
}

sub AddReducer
{
    my $self   = shift;
    my %param  = Params([qw(?Reducer ?Name ?Initialize ?Transition
			                ?Combiner ?Finalizer)], @_);
    my $reducer = $param{"Reducer"};
    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($reducer, "GPMapReduce::Reducer", "Reducer", 1);
    
    # We should have either recieved a real mapper and nothing else,
    # OR enough parameters to make one.
    if (!defined $reducer) {
		delete $param{"Reducer"};
		$reducer = new GPMapReduce::Reducer(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Reducer and other arguments");
    }

	$self->{OBJECTS}->{uc($reducer->{Name})} = $reducer;
    push @{$self->{REDUCES}}, $reducer;
    return undef;
}

sub AddTransition
{
    my $self   = shift;
    my %param  = Params([qw(?Transition  ?Name  ?Language  ?Body
			                ?Optimize    ?Input ?Output    ?Function)], @_);

    my $transition = $param{"Transition"};
    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($transition, "GPMapReduce::Transition", "Transition", 1);
    
    # We should have either recieved a real transition and nothing else,
    # OR enough parameters to make one.
    if (!defined $transition) {
		delete $param{"Transition"};
		$transition = new GPMapReduce::Transition(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Transition and other arguments");
    }

    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($transition, "GPMapReduce::Transition", "Transition", 0);

	$transition->CheckInput(undef);   # FIXME, this doesn't belong here

	$self->{OBJECTS}->{uc($transition->{Name})} = $transition;
    push @{$self->{FUNCS}}, $transition;
    return undef;
}

sub AddCombiner
{
    my $self   = shift;
    my %param  =  Params([qw(?Combiner  ?Name  ?Language  ?Body
			                 ?Optimize  ?Input ?Output    ?Function)], @_);
    my $combiner = $param{"Combiner"};

    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($combiner, "GPMapReduce::Combiner", "Combiner", 1);
    
    # We should have either recieved a real combiner and nothing else,
    # OR enough parameters to make one.
    if (!defined $combiner) {
		delete $param{"Combiner"};
		$combiner = new GPMapReduce::Combiner(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Combiner and other arguments");
    }
    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($combiner, "GPMapReduce::Combiner", "Combiner", 0);

	$self->{OBJECTS}->{uc($combiner->{Name})} = $combiner;
    push @{$self->{FUNCS}}, $combiner;
    return undef;
}

sub AddFinalizer
{
    my $self   = shift;
    my %param  = Params([qw(?Finalizer  ?Name  ?Language  ?Body
			                ?Optimize   ?Input ?Output    ?Function)], @_);
    my $finalizer = $param{"Finalizer"};

    TypeCheck($self,   "GPMapReduce::Document", "\$self", 0);
    TypeCheck($finalizer, "GPMapReduce::Finalizer", "Finalizer", 1);
    
    # We should have either recieved a real finalizer and nothing else,
    # OR enough parameters to make one.
    if (!defined $finalizer) {
		delete $param{"Finalizer"};
		$finalizer = new GPMapReduce::Finalizer(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Finalizer and other arguments")
    }

	$self->{OBJECTS}->{uc($finalizer->{Name})} = $finalizer;
    push @{$self->{FUNCS}}, $finalizer;
    return undef;
}

sub AddTask
{
	my $self  = shift;
    my %param = Params([qw(?Task ?Name ?Source ?Mapper ?Reducer ?Output)], @_);
    my $task  = $param{"Task"};

    TypeCheck($self, "GPMapReduce::Document", "\$self", 0);
    TypeCheck($task, "GPMapReduce::Task", "Task", 1);
    
    # We should have either recieved a real task and nothing else,
    # OR enough parameters to make one.
    if (!defined $task) {
		delete $param{"Task"};
		$task = new GPMapReduce::Task(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Task and other arguments")
    }

	$self->{OBJECTS}->{uc($task->{Name})} = $task;
    push @{$self->{TASKS}}, $task;
    return undef;

}

sub AddRun
{
	my $self  = shift;
    my %param = Params([qw(?Task ?Source ?Mapper ?Reducer ?Output)], @_);
    my $task  = $param{"Task"};

    TypeCheck($self, "GPMapReduce::Document", "\$self", 0);
    TypeCheck($task, "GPMapReduce::Task", "Task", 1);
    
    # We should have either recieved a real task and nothing else,
    # OR enough parameters to make one.
    if (!defined $task) {
		delete $param{"Task"};
		$param{Name} = 'Run_Task';
		$task = new GPMapReduce::Task(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify a Task and other arguments")
    }

    push @{$self->{RUN}}, $task;
    return undef;
}


################################################################

=head2 Generation

=over 4

=item GenerateSQL  

  Produces the SQL output to render the document.

=item GenerateYAML

  Unparses the document back into YAML.

=back

=cut

################################################################

sub Execute($$@)
{
	my ($self, $conn) = (shift, shift);
    my %param = Params([qw(?Explain)], @_);
	my $error = undef;
	my (@objects, $o);


    ############################################
	# Before we can use the objects we need to call CheckInput
	# This will validate that the parse was successful and that all
	# the objects link together correctly
	$self->_CheckInputs();

    #############################################
    # 1) Create objects
    # We'll create them inputs/funcs/aggs/job order
    # Theoretically functions may depend on each other if some of them
    # are sql or pgsql functions -- so we may need more infastructure here.
    @objects = (@{$self->{INPUTS}},
				@{$self->{FUNCS}},
				@{$self->{REDUCES}},
				@{$self->{TASKS}});
    foreach $o (@objects) {
#		print STDERR "Creating $o->{Name} : ";
		if (!$conn->do($o->SQLCreate($self->{PARAM}))) {
			$error = $conn->errstr;
			print STDERR "$error\n";
			last;
		}
#		print STDERR "SUCESS\n";
    }
	
	#############################################
	# 2) Prepare the tasks
	if (!$error) {
		my @runlist = @{$self->{RUN}};
		foreach my $r (@runlist) {
			my $sql = '';

			# Explain is currently a bit buggy
			# It only works correctly for target = STDOUT
			$sql = "explain " if ($param{Explain});
			$sql .= $r->BuildQuery($self->{PARAM});
			
			
			# FIXME - needs work for non STDOUT outputs
 			#       - stdout could probably use better formating
#			print STDERR "$sql\n";
			my $stmt = $conn->prepare($sql);
			$stmt->execute();
			while (my @row = $stmt->fetchrow_array()) {
				print join('|', @row) . "\n";
			}
		}
	}

	############################################
    # 3) Drop objects
    @objects = (@{$self->{TASKS}}, 
				@{$self->{REDUCES}},
				@{$self->{FUNCS}},
				@{$self->{INPUTS}});
    foreach $o (@objects) { 
#		print STDERR "Dropping $o->{Name} : ";
		if (!$conn->do($o->SQLDrop())) {
			print STDERR $conn->errstr . "\n";
			$error = $conn->errstr unless ($error);
		} else {
#			print STDERR "SUCESS\n";
		}
	}

	return $error;
}


sub GenerateSQL 
{
    my ($self, $explain) = (shift, shift);
    my ($sql, @objects, $o);

    ############################################
	# Before we can use the objects we need to call CheckInput
	# This will validate that the parse was successful and that all
	# the objects link together correctly
	$self->_CheckInputs();

    #############################################
    # 1) Create objects
    # We'll create them inputs/funcs/aggs/job order
    # Theoretically functions may depend on each other if some of them
    # are sql or pgsql functions -- so we may need more infastructure here.
	$sql = "--- CREATING OBJECTS --- \n";
    @objects = (@{$self->{INPUTS}},
				@{$self->{FUNCS}},
				@{$self->{REDUCES}},
				@{$self->{TASKS}});
    foreach $o (@objects) {
		my $create = $o->SQLCreate($self->{PARAM});
		$sql .= "$create\n" if (defined $create);
    }
    
    #############################################
    # 2) Prepare the tasks
    $sql .= "--- RUN QUERIES --- \n";
    my @runlist = @{$self->{RUN}};
    foreach my $r (@runlist) {

		# Explain is currently a bit buggy
		# It only works correctly for target = STDOUT
		$sql .= "explain " if ($explain);
		$sql .= $r->BuildQuery($self->{PARAM});
		$sql .= "\n";
    }

    ############################################
    # 3) Drop objects
	$sql .= "--- CLEANUP OBJECTS --- \n";
    @objects = (@{$self->{TASKS}}, 
				@{$self->{REDUCES}},
				@{$self->{FUNCS}},
				@{$self->{INPUTS}});
    foreach $o (@objects) {
		my $drop = $o->SQLDrop();
		$sql .= $drop if (defined $drop);
    }

    ######################### 
    # 4) Return the plan
    return $sql;
}

sub GenerateYAML
{
	Error("NOT IMPLEMENTED");
}

################################################################

=head2 Access

=over 4

=item GetDatabase()

  Returns the database the document belongs to

=item GetUser()

  Returns the user the document uses for the database

=back

=cut

################################################################

sub GetDatabase($) {
    my $self = shift;
    return $self->{"DATABASE"};
}

sub GetUser($) {
    my $self = shift;
    return $self->{"USER"};
}

sub GetHost($) {
	my $self = shift;
	return $self->{"HOST"};
}

sub GetPort($) {
	my $self = shift;
	return $self->{"PORT"};
}


################################################################
# Internal functions
################################################################
sub _CheckInputs()
{
	my $self = shift;
	my $o;
	return undef if $self->{CHECKED};
	
	# Check that the input for all objects is valid
	foreach $o (keys %{$self->{OBJECTS}}) {
		$self->{OBJECTS}->{$o}->CheckInput($self->{OBJECTS});
	}
	foreach $o (@{$self->{RUN}}) {
		$o->CheckInput($self->{OBJECTS});
	}

	$self->{CHECKED} = 1;
}


################################################################

=head1 SEE ALSO

GPMapReduce::Input, GPMapReduce::Output, GPMapReduce::Mapper 
GPMapReduce::Reducer, GPMapReduce::Driver.

=head1 AUTHORS

Caleb Welton, Jeffrey I Cohen

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: support@greenplum.com

=cut

################################################################

1;
__END__
