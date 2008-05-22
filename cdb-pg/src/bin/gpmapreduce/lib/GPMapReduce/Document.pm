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
    use GPMapReduce::Mapper;
    use GPMapReduce::Transition;
    use GPMapReduce::Combiner;
    use GPMapReduce::Finalizer;
    use GPMapReduce::Reducer;
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
    $self->{INPUTS}   = [];
    $self->{FUNCS}    = [];
    $self->{MAPS}     = [];
    $self->{REDUCES}  = [];
    $self->{JOBS}     = [];
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

=item SetVersion(version)

  Sets the version for YAML generation

=back

=cut

################################################################

sub SetDatabase() {
    my $self = shift;
    $self->{"DATABASE"} = shift;
    return undef;
}

sub SetUser() {
    my $self = shift;
    $self->{"USER"} = shift;
    return undef;
}

sub SetVersion() {
    my $self = shift;
    $self->{"VERSION"} = shift;
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

=back

=cut

################################################################

sub AddInput
{
    my $self   = shift;
    my %param  = Params([qw(?Input ?Name ?Source ?Format ?Columns 
                            ?Delimiter)], @_);
    my $input  = $param{"Input"};
    TypeCheck($self, "GPMapReduce::Document", "\$self", 0);
    TypeCheck($input, "GPMapReduce::Input", "Input", 1);

	# We should have either recieved a real mapper and nothing else,
    # OR enough parameters to make one.
    if (!defined $input) {
		delete $param{"Input"};
		$input = new GPMapReduce::Input(%param);
    } elsif (scalar keys %param > 1) {
		my @k = keys %param;
		Error("Can not specify an Input and other arguments");
    }

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

    push @{$self->{FUNCS}}, $finalizer;
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

sub GenerateSQL 
{
    my $self    = shift;
    my $explain = shift;
    my $sql     = "";
    my @objects;
	my $o;
	
    ############################################
    # 1) Drop objects, just in case
    # First drop any objects that we are defining; this is just in case
    # we had prior definitions that have changed.  It would, of course,
    # be better if we could check them first.  
    # Delete the objects job/aggs/funcs/inputs order

    @objects = (@{$self->{JOBS}}, 
				@{$self->{REDUCES}},
				@{$self->{FUNCS}},
				@{$self->{INPUTS}});
    foreach $o (@objects) {
		my $drop;
		if (isa($o, "GPMapReduce::Function") or
			isa($o, "GPMapReduce::Input") or
			isa($o, "GPMapReduce::Reducer"))
		{
			$drop = $o->GenerateSQL_Drop();
		} else {
			print "[WARN] needs update";
			$drop = $o->{DROP};
		}
		$sql .= $drop if (defined $drop);
    }

    #############################################
    # 2) Create objects
    # We'll create them inputs/funcs/aggs/job order
    # Theoretically functions may depend on each other if some of them
    # are sql or pgsql functions -- so we may need more infastructure here.

    @objects = (@{$self->{INPUTS}},
				@{$self->{FUNCS}},
				@{$self->{REDUCES}},
				@{$self->{JOBS}});
    foreach $o (@objects) {
		my $create;
		if (isa($o, "GPMapReduce::Function") or
			isa($o, "GPMapReduce::Input") or
			isa($o, "GPMapReduce::Reducer"))
		{
			$create = $o->GenerateSQL_Create();
		} else {
			$create = $o->{CREATE};
		}
		$sql .= $create if (defined $create);
    }
    
    #############################################
    # 3) Prepare the jobs

    my @runlist = @{$self->{RUN}};
    foreach my $r (@runlist) {

	# Explain is currently a bit buggy
	# It only works correctly for target = STDOUT
	$sql .= "explain " if ($explain);
	$sql .= $r->{RUN};
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


sub add_split {

}

sub add_job($$$$$$$) {
	my $self    = shift;
	my $name    = shift;
	my $source  = shift;
	my $map     = shift;
	my $reduce  = shift;
	my $group   = shift;
	my $opt     = shift;
	my $drop;
	my $create;

	return "UNNAMED JOB"                     if (!defined($name));
	return "JOB <$name> is not a valid name" unless _checkname($name);
	return "JOB <$name> must have a valid source"
		unless (defined($source) and _checkname($source));
	return "JOB <$name> cannot have grouper and reducer"
		if (defined($group) and defined($reduce));
	$group = "IDENTITY" unless (defined($group) or defined($reduce));
	return "JOB <$name> must have a valid grouper"
		unless(!defined($group) or _checkname($group));
	return "JOB <$name> must have a valid reducer"
		unless(!defined($reduce) or _checkname($reduce));

	$drop    = "DROP VIEW $name CASCADE;\n";
	$create  = "CREATE VIEW $name as \n";
	$create .= " FIXME - needs implementation\n";

	push @{$self->{"JOBS"}}, {
		"NAME"       => $name,
		"SOURCE"     => $source,
		"MAPPER"     => $map,
		"REDUCER"    => $reduce,
		"GROUPER"    => $group,
		"OPTIMIZE"   => $opt,
		"DROP"       => $drop,
		"CREATE"     => $create,
	};
	undef;
}

sub add_run($$$$$$$) {
	my $self    = shift;
	my $source  = shift;
	my $target  = shift;
	my $map     = shift;
	my $reduce  = shift;
	my $mode    = shift;
	my @param;
	my $key;
	my $type;
	my $path;
	my $run;
	my $map_item;
	my $source_item;
	
	$map    = "IDENTITY" unless(defined($map));
	$reduce = "IDENTITY" unless(defined($reduce));
	$target = "STDOUT"   unless(defined($target));

	return "EXECUTION has invalid map name <$map>"
		unless (_checkname($map));
	return "EXECUTION has invalid reduce name <$reduce>"
		unless (_checkname($reduce));

	if (uc($target) eq "STDOUT") 
	{
		$type = "PIPE";
		$path = "";
	}
	else 
	{
		return "EXECUTION has invalid target name <$target>"
			unless ($target =~ /^(table|file):\/\/(.*)/i);
		$type = uc($1);
		$path = $2;
	}

	@param = @{$self->{"PARAM"}};
	if ($#param >= 0) 
	{
		$key = $param[0];
		$key =~ s/'/''/g;         # escape quotes #'
		$key = "'$key'";          # wrap in quotes
	}
	else
	{
		$key = "NULL";
	}


	# Look for the MAP in our list of functions
	if (uc($map) ne "IDENTITY")
	{
		my @funcs = @{$self->{FUNCS}};
		foreach my $f (@funcs) {
			if (isa($f, "GPMapReduce::Mapper") and
				uc($f->{Name}) eq uc($map)) {
				$map_item = $f;
				last;
			}
		}
		# if no mapfunc we will assume it is a pre-defined function
		# WARNING: this may cause problems in the future
	}

	$run = "";
	$run .= "CREATE TABLE $path AS\n" if ($type eq "TABLE");
	$run .= "COPY (\n" if ($type eq "FILE");
	$run .= "SELECT ";

	$run .= "key, " unless (uc($map) eq "IDENTITY");

	if (uc($reduce) eq "IDENTITY")
	{
		$run .= "value\n";
	}
	else
	{
		$run .= "$reduce(value) as value\n";
	}

	if (uc($map) eq "IDENTITY")
	{
		$run .= "FROM $source\n";
	}
	else
	{
		$run .= "FROM (SELECT key(m), value(m) FROM (";
		$run .= "SELECT $map($key, value) as m FROM $source) q1) q2\n";
	}

	if (uc($reduce) eq "IDENTITY")
	{
		if (uc($map) eq "IDENTITY")
		{
			$run .= "ORDER BY value\n";
		}
		else
		{
			$run .= "WHERE key is not null\n";
			$run .= "ORDER BY key, value\n";
		}
	}
	else
	{
		if (uc($map) ne "IDENTITY")
		{
			$run .= "WHERE key is not null\n";
			$run .= "GROUP BY key\n";
			$run .= "ORDER BY key\n";
		}
	}
	$run .= ") to '/$path'" if ($type eq "FILE");
	$run .= ";\n";

	push @{$self->{"RUN"}}, {
		"SOURCE"     => $source,
		"TARGET"     => $target,
		"MAPPER"     => $map,
		"REDUCER"    => $reduce,
		"MODE"       => $mode,
		"RUN"        => $run,
	};
	undef;
}

sub _checkname($)
{
    my $name = shift;
    return 1 if ($name =~ /^[a-zA-Z]\w*$/);
    return 0;
}

################################################################

=head1 SEE ALSO

GPMapReduce::Input, GPMapReduce::Output, GPMapReduce::Mapper 
GPMapReduce::Reducer, GPMapReduce::Driver.

=head1 AUTHORS

Caleb Welton, Jeffery I Cohen

Copyright (c) 2008 GreenPlum. All rights reserved.

Address bug reports and comments to: cwelton@greenplum.com

=cut

################################################################

1;
__END__
