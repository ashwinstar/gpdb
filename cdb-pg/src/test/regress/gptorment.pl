#!/usr/bin/perl
#
# $Header: /Users/jcohen/perl/RCS/explain.pl,v 1.3 2006/11/06 23:32:06 jcohen Exp jcohen $
#
# copyright (c) 2008
# Author: Jeffrey I Cohen
#
#
use Pod::Usage;
use Getopt::Long;
use Data::Dumper;
use Config;
use strict;
use warnings;

=head1 NAME

B<gptorment.pl> - subject database to infernal torment

=head1 SYNOPSIS

B<gptorment.pl> [options] 

Options:

    -help            brief help message
    -man             full documentation
    -connect         psql connect parameters
    -sqlfile         the test file
    -id              id of parent (DO NOT SET)
    -parallel        degree of parallelism/concurrency
    -iter            number of testing iterations
    -timelimit       time duration for time-limited testing
    -debug           debug flag (show test plan, do not execute)
    -tmpdir          temporary directory (DO NOT SET)
    -grace_period    time to wait before killing child test processes

=head1 OPTIONS

=over 8

=item B<-help>

    Print a brief help message and exits.

=item B<-man>
    
    Prints the manual page and exits.

=item B<-connect>
    
    psql connect string, e.g:

    -connect '-p 11000 template1'
    
=item B<-sqlfile>

    A sql file to execute.

=item B<-id>

    The ID code for the parent of the current test process.  DO NOT SET.
    
=item B<-parallel>
    
    The degree of parallelism of testing.  Spawn this many concurrent
    child test processes.  Degree=1 if not set.

=item B<-iter>

    The number of test iterations (1 by default). Note that if only
    iter is set and not timelimit, then gptorment has no way to detect
    test failures like infinite loops or hangs.

=item B<-timelimit>

    For timed testing, the amount of time spent repeating the test.
    If iter is set, then run the test for (iter*timelimit) time.
    If no units are specified, the timelimit is in seconds.  Timelimit
    accepts mixed time specifications like -tl="1 min 14 seconds" or
    even -tl="2+2 min - 14 seconds"

    Legal time periods are seconds, minutes, hours, days, weeks, years
    (but not months!).  gptorment simply uses the first letter of the
    unit to determine the period.
    
=item B<-debug>

    If specified, gptorment prints the basic test plan, but does not
    execute it.
    
=item B<-tmpdir>

    The temporary directory containing all test output (DO NOT SET).
    
=item B<-grace_period>

    If a timelimit is specified, the grace period is the amount of
    time gptorment waits after the timelimit is exceeded before trying
    to B<kill -9> the child processes.  A grace period can be negative,
    which will cause gptorment to kill the children prematurely.  Note
    that if you use a negative number, you must quote the expression
    e.g. -grace='-10 sec' to prevent gptorment from confusing the
    leading minus with a command argument.

=back

=head1 DESCRIPTION

gptorment.pl is a stress test framework.
Substitute the correct "connect string" for your postgres database.

gptorment.pl repeated executes a test plan against the database for a
number of iterations or for a specified amount of time, using a
specified degree of concurrency.  It is designed for batch or
interactive use.  Because this type of testing can result in serious
failure, deadlocks, or other system resource issues, gptorment has a
simple but robust strategy to manage child processes.  Rather than
wait for child processes, gptorment detaches them, but it marks a
unique id in the process argument list.  gptorment uses "ps" to
monitor the state of the child processes, and it can clean up "stuck"
processes with kill -9.

When gptorment is invoked, it creates a uniquely-named temporary
directory to hold test output.  All the child processes write their
output to files in this directory.  

gptorment does not create or startup a database.

-head1 TODO

Solaris port

=head1 AUTHORS

Jeffrey I Cohen

Copyright (c) 2008 GreenPlum.  All rights reserved.  

Address bug reports and comments to: jcohen@greenplum.com


=cut

my $glob_id = "";

my $glob_tab;
my $glob_connect;
my $glob_paral;
my $glob_uid;
my $glob_sqlfile;
my $glob_iter;
my $glob_tlim;
my $glob_debug;
my $glob_tmpdir;
my $glob_grace;

sub gettimelimit
{
    my $tlim = shift;

    return undef
        unless (defined($tlim));

    # time limit in seconds or other units (but not "1hr10min")
    #              time [space] [timeunit]{lookahead}
    unless ($tlim =~ m/^\s*(\+|\-|\.|\()?\d+/)
    {
        return undef;
    }

    $tlim =~ s/\s+//gm; # no spaces

# XXX XXX: month

    $tlim =~ s/([a-zA-Z])([a-zA-Z])*/$1/gm;

#    print "tlim: $tlim\n";

    $tlim = lc($tlim);

    my @foo = split(/([a-z])/, $tlim);

    $tlim = 0;

    my $tempnum;

    for my $vv (@foo)
    {
        next 
            unless (length($vv));

        if ($vv !~ /^[a-z]$/)
        {
            my $estr = '$tempnum = 0 + (' . $vv . ');';

 #           print "e:",$estr, "\n";

            eval $estr;
            if ($@)
            {
                warn "invalid numeric format \'$vv\' in time specification\n";
                return undef;
            }
        }
        else
        {
            my $min = 60;
            my $hr = $min * 60;
            my $day = $hr * 24;
            my $wk = $day * 7;
            my $mon = $day * 30;
            my $yr = $day * 365;

            if ($vv =~ m/s/)
            {
                $tlim += ($tempnum) 
            }
            elsif ($vv =~ m/m/)
            {
                $tlim += ($tempnum*$min) 
            }
            elsif ($vv =~ m/h/)
            {
                $tlim += ($tempnum*$hr) 
            }
            elsif ($vv =~ m/d/)
            {
                $tlim += ($tempnum*$day) 
            }
            elsif ($vv =~ m/w/)
            {
                $tlim += ($tempnum*$wk) 
            }
# XXX XXX: month
            elsif ($vv =~ m/y/)
            {
                $tlim += ($tempnum*$yr) 
            }
            else
            {
                warn "unknown time period spec \'$vv\'";
                return undef;
            }
            $tempnum = 0;

        }

    }

    $tlim += $tempnum;

#    print join(":", @foo), "\n";
#    print "time limit: $tlim \n";

    return $tlim;

} # end gettimelimit

sub I_am_interactive 
{
    return -t STDIN ;
}

sub gettmpfile
{

} # end gettmpfile

sub gettmpdir
{
    use File::Temp qw/ tempfile tempdir /;

    my $id = shift;

    # try to remove the leading date/time string from the id (to save
    # some space), since it will be implicit in the directory creation
    # time
    my @foo = split(/\./, $id, 2);

    if (2 == scalar(@foo))
    {
        shift @foo;
        $id = pop(@foo);
    }

    # replace dots with underscores
    $id =~ s/\./\_/gm;

    my $template = "gptorment_" . $id . "_XXXXXX";

    my $tempdir = tempdir ( $template, TMPDIR => 1, SUFFIX => '.dir' );

    return $tempdir;
} # end gettmpdir

BEGIN {
    my $man  = 0;
    my $help = 0;
    my $table;
    my $conn;
    my $paral;
    my $uid;
    my $sqlfile;
    my $iter;
    my $tlim;
    my $dbg;
    my $tmpdir;
    my $grace;

    GetOptions(
               'help|?' => \$help, man => \$man, 
               "table=s" => \$table,
               "connect=s" => \$conn,
               "id:s" => \$uid,
               "parallel:i" => \$paral,
               "sqlfile:s" => \$sqlfile,
               "iter:i" => \$iter,
               "tlim|timelimit:s" => \$tlim,
               "dbg|debug:s" => \$dbg,
               "tmpdir|tempdir:s" => \$tmpdir,
               "grace_period|grace_time|graceperiod|gracetime:s" => \$grace,
               )
        or pod2usage(2);

    
    pod2usage(-msg => $glob_id, -exitstatus => 1) if $help;
    pod2usage(-msg => $glob_id, -exitstatus => 0, -verbose => 2) if $man;

    $glob_tab = $table;
    $glob_connect = $conn;

    $glob_connect = '-p 11000 template1'
        unless (defined($glob_connect));

    $paral = 1
        unless (defined($paral));

    unless (defined($uid))
    {

        my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) =
            localtime(time);

        $year += 1900;
        $mon++;

        $uid = sprintf("%04d%02d%02d%02d%02d%02d",
                       $year,$mon, $mday, $hour, $min ,$sec);
    }

    $uid .= "." . $$;

    $iter = 1
        unless (defined($iter));

    unless (defined($sqlfile))
    {
        $glob_id = "no sql file";

        pod2usage(-msg => $glob_id, -exitstatus => 1);
    }

    if (defined($tlim))
    {
        my $orig_tlim = $tlim;

        $tlim = gettimelimit($tlim);

        unless (defined($tlim))        
        {
            $glob_id = "invalid timelimit specification: $orig_tlim";
        
            pod2usage(-msg => $glob_id, -exitstatus => 1);
        }
        if ($tlim <= 0)
        {
            warn "invalid negative time $tlim seconds";
            pod2usage(-msg => $glob_id, -exitstatus => 1);
        }
    }

    unless (defined($tmpdir))
    {
        $tmpdir = gettmpdir($uid);
    }

    if (defined($grace))
    {
        my $orig_tlim = $grace;

        $grace = gettimelimit($grace);

        unless (defined($grace))        
        {
            $glob_id = "invalid grace_limit specification: $orig_tlim";
        
            pod2usage(-msg => $glob_id, -exitstatus => 1);
        }
        # NOTE:  allow negative grace period
    }
    else
    {
        $grace = 10;
    }


    $glob_uid = $uid;
    $glob_paral = $paral;
    $glob_iter = $iter;
    $glob_sqlfile = $sqlfile;
    $glob_tlim = $tlim;
    $glob_debug = $dbg;
    $glob_tmpdir = $tmpdir;
    $glob_grace = $grace;

#    print "loading...\n" ;
}

# convert a postgresql psql formatted table into an array of hashes
sub tablelizer
{
    my $ini = shift;

    # first, split into separate lines, the find all the column headings

    my @lines = split(/\n/, $ini);

    return undef
        unless (scalar(@lines));

    my $line1 = shift @lines;

    # look for <space>|<space>
    my @colheads = split(/\s+\|\s+/, $line1);

    # fixup first, last column head (remove leading,trailing spaces)

    $colheads[0] =~ s/^\s+//;
    $colheads[0] =~ s/\s+$//;
    $colheads[-1] =~ s/^\s+//;
    $colheads[-1] =~ s/\s+$//;

    return undef
        unless (scalar(@lines));
    
    shift @lines; # skip dashed separator

    my @rows;

    for my $lin (@lines)
    {
        my @cols = split(/\|/, $lin, scalar(@colheads));
        last 
            unless (scalar(@cols) == scalar(@colheads));

        my $rowh = {};

        for my $colhd (@colheads)
        {
            my $rawcol = shift @cols;

            $rawcol =~ s/^\s+//;
            $rawcol =~ s/\s+$//;

            $rowh->{$colhd} = $rawcol;
        }
        push @rows, $rowh;
    }

    return \@rows;
}

sub do_ps_pids
{
    my $uid = shift;

    my $pidlist = [];
    my $pidh = {};

    if ($Config{'osname'} =~ m/solaris|sunos/i)
    {
        my $cmd = 'ps -elf -o pid,args | grep -v grep | grep gptorment ';

        my $psout = `$cmd`;

        my @lines = split(/\n/, $psout);

        for my $lin (@lines)
        {
            my @foo = split(/\s+/, $lin, 2);
        
            next 
                unless (2 == scalar(@foo));

            my $pid = shift @foo;

            # on solaris, ps args are truncated, so use pargs to get args
            my $pidpargs = `pargs $pid`;

            next
                unless (defined($pidpargs)
                        && ($pidpargs =~ m/$uid/m));
        
            push @{$pidlist}, $pid;

            $pidh->{$pid} = 1;

        }
    }
    else # linux or osx
    {

        my $cmd = 'ps auxww | grep -v grep | grep gptorment | grep ' . $uid;
   
        my $psout = `$cmd`;

        my @lines = split(/\n/, $psout);

        for my $lin (@lines)
        {
            my @foo = split(/\s+/, $lin, 3);
        
            next 
                unless (3 == scalar(@foo));

            shift @foo;

            my $pid = shift @foo;
        
            push @{$pidlist}, $pid;

            $pidh->{$pid} = 1;

        }
    }
    return ($pidh, $pidlist);
} # end do_ps_pids


sub init_monitor
{
    my ($uid, $iter, $tlim, $paral) = @_;

    my $mon_stat = {};

    $mon_stat->{uid}   = $uid;
    $mon_stat->{iter}  = $iter;
    $mon_stat->{tlim}  = $tlim;
    $mon_stat->{paral} = $paral;

    my ($pidh, $pidlist) = do_ps_pids($uid);
 
    $mon_stat->{orig_pidlist} = $pidlist;
    $mon_stat->{curr_pidlist} = [];
    push @{$mon_stat->{curr_pidlist}}, @{$pidlist};
    $mon_stat->{pidh} = $pidh;
    
    if ($paral != scalar(@{$pidlist}))
    {
        $mon_stat->{msg} = "WARNING: found " . scalar(@{$pidlist}) . 
            " processes, expected " . $paral . ".\n";
    }
    else
    {
        $mon_stat->{msg} = "Found " . $paral . " processes.\n"
    }

#    print Data::Dumper->Dump([$mon_stat]);

    return $mon_stat;
    
}

sub do_monitor
{
    my ($mon_stat, $tim) = @_;

    my ($pidh, $pidlist) = do_ps_pids($mon_stat->{uid});

    if (exists($mon_stat->{msg}))
    {
        print $mon_stat->{msg};
        delete $mon_stat->{msg};
    }

    my $pcount = scalar(@{$pidlist});
    my $old_pcount = scalar(@{$mon_stat->{curr_pidlist}});

    if ($pcount < $old_pcount)
    {
        print time(), ": ", $old_pcount - $pcount, " processes died, $pcount remaining\n";
    }
    elsif ($pcount > $old_pcount)
    {
        print time(), ": ", $pcount - $old_pcount, " processes BORN, $pcount now remaining\n";

    }
    else
    {
        print time(), ": ", $pcount, " processes still remaining\n";
    }

    $mon_stat->{curr_pidlist} = $pidlist;
    $mon_stat->{pidh} = $pidh;

    sleep ($tim)
        if ($pcount);

    return $pcount;
}

sub do_kill
{
    my ($uid, $iter, $tlim, $paral) = @_;

    my $mon_stat = init_monitor($uid, $iter, $tlim, $paral);

    my $maxtime = time() + ($glob_iter * $glob_tlim) + $glob_grace;

    while (do_monitor($mon_stat, 10) && (time() < $maxtime))
    {

    }

    # kill if necessary

    do_monitor($mon_stat, 0);

    my $pcount = scalar(@{$mon_stat->{curr_pidlist}});

    print time(), ": $pcount processes remain at test end\n";

    for my $pid (@{$mon_stat->{curr_pidlist}})
    {
        print "killing $pid\n";
        system ("kill -9 $pid");
    }

    my $time_diff = $maxtime - time();

    if ($glob_grace > 0)
    {
        if ($time_diff <= $glob_grace)
        {
            print "test ended on time (grace period $glob_grace)\n";
        }
        else
        {
            print "test ended $time_diff seconds prematurely\n";
        }
    }
    else
    {
        my $grt = (-1 * $glob_grace);

        if ($time_diff <= $grt)
        {
            print "test ended on time (negative grace period $glob_grace)\n";
        }
        else
        {
            print "test ended ", ($time_diff - $grt),
            " seconds prematurely\n";
        }
    }

} # end do_kill


sub do_sql
{
    my $psql_str = "psql ";

    $psql_str .= $glob_connect
        if (defined($glob_connect));

#    $psql_str .= " -c \'\\d $glob_tab \'";
#    $psql_str .= " -c \'select * from gp_configuration \'";

    $psql_str .= " -a -e  -f $glob_sqlfile";

#    print $psql_str, "\n";

    my $tstout = "gpt_" . $glob_uid;

    $tstout =~ s/\./\_/gm;

    $tstout .= ".out";

    $tstout = File::Spec->catfile($glob_tmpdir, $tstout);

    my $tstfh;

    open ($tstfh, ">$tstout" ) or die "can't open $tstout: $!";

    if (($glob_paral == 1) && (!defined($glob_debug)))
    {
        my $start_time = time();
        my $end_time;
        $end_time = $start_time + $glob_tlim
            if (defined($glob_tlim));

#        print "sql tlim: $glob_tlim\n"
#            if (defined($glob_tlim));

        my $realSTDOUT;

        open $realSTDOUT, ">&STDOUT"  or die "Can't dup STDOUT: $!";

        for my $ii (1..$glob_iter)
        {
            my $filnam;

            $filnam = "sql_" . $glob_uid . "_" . $ii;
            
            $filnam =~ s/\./\_/gm;

            $filnam .= ".out";

            my $tmpnam = File::Spec->catfile($glob_tmpdir, $filnam);

            close STDOUT;
            open (STDOUT, ">$tmpnam" ) or die "can't open STDOUT: $!";

            select STDOUT; $| = 1;      # make unbuffered
            
            my $tl_iter = 0;

          l_timeloop:

            my $tabdef = `( $psql_str ) 2>&1`;

            print $tabdef;
            print $tstfh $tabdef;

            if (defined($end_time))
            {
                if (time() < $end_time)
                {
                    $tl_iter++;
                    print $tstfh "-- GP_IGNORE: timeloop $tl_iter, ", 
                    time(), "\n";
                    goto l_timeloop;
                }

#            undef $end_time;
                $end_time = time() + $glob_tlim;
            }
        }

        if (defined($realSTDOUT))
        {
            close STDOUT;
            open STDOUT, ">&", $realSTDOUT or die "Can't dup \$realSTDOUT: $!";
            
        }

    }
    else
    {
        my $do_para = $glob_paral;

        if (defined($glob_debug))
        {
            my $timetxt = localtime();
            print "# start at $timetxt\n";
            print "# repeat $do_para times...\n";
            $do_para = 1;
        }

        for my $ii (1..$do_para)
        {
            my $cmd;

            $cmd = $0 . " ";

            $cmd .= "--id=" . $glob_uid . " ";
            $cmd .= "--conn=\'" . $glob_connect . "\' "
                if (defined($glob_connect));
            $cmd .= "--iter=" . $glob_iter . " ";
            $cmd .= "--sqlfile=" . $glob_sqlfile . " ";

            $cmd .= "--timelimit=\'" . $glob_tlim . " seconds\' "
                if (defined($glob_tlim));

            $cmd .= "--tempdir=\'" . $glob_tmpdir . "\' "
                if (defined($glob_tmpdir));

            $cmd .= "--grace_period=\'" . $glob_grace . " seconds\' ";

            $cmd .= " & ";

            print $cmd, "\n";

            unless (defined($glob_debug))
            {
                print $tstfh $cmd, "\n";
                system($cmd);
            }

        } # end for ii

        unless (defined($glob_debug))
        {
            if (defined($glob_tlim))
            {
                do_kill($glob_uid, $glob_iter, $glob_tlim, $glob_paral);
            }
            else
            {
                my $mon_stat = init_monitor($glob_uid, $glob_iter, $glob_tlim,$glob_paral);
                while (do_monitor($mon_stat, 10))
                {

                }
            }
        }


    } # end else

    close $tstfh;

} # end do_sql

if (1)
{
    if (defined($glob_sqlfile)
        && length($glob_sqlfile))
    {
        do_sql();
    }

}

exit();

