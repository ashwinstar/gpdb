
This directory contains component level testing for the amsd component.


Running the Tests:
-----------------

Unfortunately, these tests are not integrated into the build procedure so 
you need to BE VERY CAREFUL TO FOLLOW THESE STEPS:

0.  have your environment setup so that path points to the cdb executables
    created from the make process.
 
1.  from cdb-pg do a:

   make install

2.  from this directory do a:

   make

3.  from this directory do a :
 
   ./runtest.sh 


The runtest.sh will: 
  1) launch amsd
  2) launch test-amsd  (which is the component test)
  3) kill amsd 

test-amsd writes the test results to standard out.  All tests should Pass


