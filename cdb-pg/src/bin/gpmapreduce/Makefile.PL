use 5.006;
use ExtUtils::MakeMaker;

WriteMakefile(
              NAME              => 'GPMapReduce',
              AUTHOR            => 'Caleb Welton <cwelton@greenplum.com>',
              ABSTRACT_FROM     => 'lib/GPMapReduce/Parser.pm',
              VERSION_FROM      => 'lib/GPMapReduce/Parser.pm',
              PREREQ_PM         => {
                  'Test::More'     => '0',
                  'Getopt::Long'   => '0',
                  'Pod::Usage'     => '0',
                  'YAML::Tiny'     => '1.25'
                  },
              
              'EXE_FILES' =>
	              [ 'bin/mapred.pl' ], # install in /usr/bin
              );


