# Replace these lines with whatever is needed to get the
# right path and environment variables for the build.
#
# These lines assume we are using Cygwin's bison and flex
# for the build, and Cygwin is in c:\cgywin
# so all we are doing is adding cygwin to the path
#
# The other environment variables needed to build
# would have been set by vcvarsall.bat before running
# the perl code.
#
# either 
#     vcvarsall.bat x86
# or
#     vcvarsall.bat amd64

$ENV{'CYGWIN'} = 'nodosfilewarning';
$ENV{'PATH'} = $ENV{'PATH'} . ';c:\cygwin\bin';


