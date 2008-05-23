use 5.006;
use ExtUtils::MakeMaker;

{
    print "Testing for YAML...\n";

    if (eval 'require YAML::Tiny')
    {
        print "required package YAML::Tiny is installed\n";
    }
    elsif (eval 'require YAML')
    {
#        print "required package YAML is installed\n";
    }
    else
    {
        warn("Warning: The required package YAML::Tiny is not installed -- please download it from www.cpan.org\n");
    }
}

my $psql_t1 = `psql -V`;

my $glob_connect = " postgres ";

# META fixup: if a connect string is supplied, use it to fix up fixup.pl
if (scalar(@ARGV))
{
    $glob_connect = shift @ARGV;

    my $plname = $0;

    my @foo = File::Spec->rel2abs(File::Spec->splitpath($plname));

    die ("could not fix up fixup.pl")
        unless (scalar(@foo));

    pop @foo;

    $fixup_script =
        File::Spec->rel2abs(
                            File::Spec->catfile(
                                                @foo,
                                                bin,
                                                "fixup.pl"
                                                ));

    my $gen_connect = '\_\_CONNECT: ' . $glob_connect . ' \_\_';

#    print $gen_connect, "\n";

    my $regexp = '\"\_\_CONNECT\:(.*)\_\_\"';

    system "perl -i -ple \' s/$regexp/\"$gen_connect\"/gm ; \' $fixup_script";

 
}

print "Testing psql...\n";

my $stat1 = ($psql_t1 =~ m/psql\s*\(PostgreSQL\)/); 

if (!$stat1)
{
    warn("Warning: psql executable not found\n");
}
else
{
    my $psql_t2 = `psql $glob_connect -t -c \'select version(); \' `;

    $stat1 = ($psql_t2 =~ m/PostgreSQL.*Greenplum/);

    warn("Warning: psql connection failure\n")
        unless ($stat1);
}

if (!$stat1)
{
    warn("Warning: cannot check for pl languages\n")
}
else
{
    my $psql_t2 = `psql $glob_connect -t -c \'select lanname from pg_language where lanispl is true ; \' `;

    $stat1 = ($psql_t2 =~ m/plperl/);

    if (!$stat1)
    {
        warn("Warning: plperl not installed\n");
    }
    else
    {
        print "Found installed languages:\n";
        print $psql_t2, "\n";
    }

}


WriteMakefile(
              NAME              => 'GPMapReduce',
              AUTHOR            => 'Caleb Welton <cwelton@greenplum.com>',
              ABSTRACT_FROM     => 'lib/GPMapReduce/Parser.pm',
              VERSION_FROM      => 'lib/GPMapReduce/Parser.pm',
              PREREQ_PM         => {
                  'Test::More'     => '0',
                  'Getopt::Long'   => '0',
                  'Pod::Usage'     => '0',
#                  'YAML::Tiny'     => '1.25'
                  },
              
              'EXE_FILES' =>
	              [ 'bin/mapred.pl' ], # install in /usr/bin
              );


